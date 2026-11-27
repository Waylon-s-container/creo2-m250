#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ProDimension.h>
#include <ProFeatType.h>
#include <ProMdl.h>
#include <ProMdlUnits.h>
#include <ProModelitem.h>
#include <ProUtil.h>
#include <ProWindows.h>
#include "bridge.h"

#define REQUEST_SIZE 512
#define RESPONSE_SIZE 65536
#define ERR_OUTSIDE_SESSION -10001
#define ERR_WRONG_UNITS -10002
#define ERR_OUTPUT_LIMIT -10003
#define ERR_DIRTY_RELOAD -10004
#define DEFER_RELOAD -10005
#define ERR_DIMENSION_RECOVERY -10006
#define ERR_NON_LENGTH_DIMENSION -10007
#define ERR_STALE_REQUEST -10008

static ProPath session_path, model_path, queue_path, template_path, ready_path;
static char session_epoch[33];
static ProName tracked_name, reload_name;
static ProMdl tracked_model;
static wchar_t reload_reply[PRO_PATH_SIZE];
static UINT_PTR timer_id;
static int busy;
static const char *current_stage = "initialize";
static _locale_t wire_locale;
static char response[RESPONSE_SIZE];
static size_t response_length;
static int response_full;

void BridgeSetStage(const char *stage)
{
    current_stage = stage;
}

static void ReplyReset(void)
{
    response[0] = '\0';
    response_length = 0;
    response_full = 0;
}

static void ReplyAdd(const char *format, ...)
{
    int count;
    va_list arguments;
    if (response_full) return;
    va_start(arguments, format);
    count = _vsnprintf_s_l(response + response_length, RESPONSE_SIZE - response_length,
                           _TRUNCATE, format, wire_locale, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= RESPONSE_SIZE - response_length) {
        response_full = 1;
        return;
    }
    response_length += count;
}

static void ReplyWideString(const wchar_t *value)
{
    ReplyAdd("\"");
    for (; *value && !response_full; ++value) {
        if (*value == L'"' || *value == L'\\') ReplyAdd("\\%c", (char)*value);
        else if (*value >= 32 && *value <= 126) ReplyAdd("%c", (char)*value);
        else ReplyAdd("\\u%04x", (unsigned)*value);
    }
    ReplyAdd("\"");
}

static int JoinPath(ProPath result, const wchar_t *directory, const wchar_t *name)
{
    return _snwprintf_s(result, PRO_PATH_SIZE, _TRUNCATE, L"%ls\\%ls", directory, name) >= 0;
}

static int WriteAtomic(const wchar_t *destination)
{
    ProPath temporary;
    FILE *stream;
    int written;
    if (_snwprintf_s(temporary, PRO_PATH_SIZE, _TRUNCATE, L"%ls.tmp", destination) < 0) return 0;
    if (_wfopen_s(&stream, temporary, L"wb") != 0) return 0;
    written = fwrite(response, 1, response_length, stream) == response_length;
    written = (fclose(stream) == 0) && written;
    if (written) written = MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!written) DeleteFileW(temporary);
    return written;
}

static int ParseName(const char *text, ProName name)
{
    size_t index;
    size_t count = strlen(text);
    if (!count || count > 30 || text[0] < 'a' || text[0] > 'z') return 0;
    for (index = 0; index < count; ++index) {
        if (!(text[index] >= 'a' && text[index] <= 'z') &&
            !(text[index] >= '0' && text[index] <= '9') && text[index] != '_') return 0;
        name[index] = (wchar_t)text[index];
    }
    name[count] = L'\0';
    if (!strcmp(text, "con") || !strcmp(text, "prn") || !strcmp(text, "aux") ||
        !strcmp(text, "nul") || !strcmp(text, "mmns_part_solid")) return 0;
    return count != 4 || (strncmp(text, "com", 3) && strncmp(text, "lpt", 3)) ||
           text[3] < '0' || text[3] > '9';
}

static int ParseInteger(const char *text, int minimum, int *output)
{
    unsigned long parsed;
    char *end;
    if (*text < '0' || *text > '9') return 0;
    parsed = strtoul(text, &end, 10);
    if (*end || parsed > INT_MAX || parsed < (unsigned long)minimum) return 0;
    *output = (int)parsed;
    return 1;
}

static int ParseLength(const char *text, int signed_coordinate, double *output)
{
    char *end;
    double value = _strtod_l(text, &end, wire_locale);
    if (end == text || *end || !_finite(value) || fabs(value) > 100000 ||
        (!signed_coordinate && value <= 0)) return 0;
    *output = value;
    return 1;
}

static int FindSavedPart(const wchar_t *name, ProPath selected)
{
    ProPath pattern, name_stem;
    WIN32_FIND_DATAW entry;
    HANDLE search;
    size_t stem_length;
    unsigned long latest = 0;
    int found = 0;
    if (_snwprintf_s(name_stem, PRO_PATH_SIZE, _TRUNCATE, L"%ls.prt", name) < 0) return 0;
    stem_length = wcslen(name_stem);
    if (_snwprintf_s(pattern, PRO_PATH_SIZE, _TRUNCATE, L"%ls\\%ls*", model_path, name_stem) < 0) return 0;
    search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        const wchar_t *suffix = entry.cFileName + stem_length;
        unsigned long revision = 0;
        wchar_t *end;
        if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            _wcsnicmp(entry.cFileName, name_stem, stem_length)) continue;
        if (*suffix) {
            if (suffix[0] != L'.' || suffix[1] < L'0' || suffix[1] > L'9') continue;
            revision = wcstoul(suffix + 1, &end, 10);
            if (*end) continue;
        }
        if (!found || revision > latest) {
            found = JoinPath(selected, model_path, entry.cFileName);
            latest = revision;
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return found;
}

static ProError ConfirmMetric(ProMdl model)
{
    ProUnitsystem system;
    ProUnititem length, millimeters;
    ProUnitConversion conversion = {0};
    ProError result;
    ProName millimeter_name = L"mm";
    BridgeSetStage("check_millimeter_units");
    result = ProMdlPrincipalunitsystemGet(model, &system);
    if (result == PRO_TK_NO_ERROR) result = ProUnitsystemUnitGet(&system, PRO_UNITTYPE_LENGTH, &length);
    if (result == PRO_TK_NO_ERROR && !_wcsicmp(length.name, L"mm")) return PRO_TK_NO_ERROR;
    if (result == PRO_TK_NO_ERROR) result = ProUnitInit(model, millimeter_name, &millimeters);
    if (result == PRO_TK_NO_ERROR) result = ProUnitConversionCalculate(&length, &millimeters, &conversion);
    if (result != PRO_TK_NO_ERROR) return result;
    return fabs(conversion.scale - 1.0) < 1e-9 && fabs(conversion.offset) < 1e-9
        ? PRO_TK_NO_ERROR : ERR_WRONG_UNITS;
}

static ProError DisplayTracked(ProMdl model, const wchar_t *name)
{
    ProError result;
    int window;
    result = ConfirmMetric(model);
    if (result != PRO_TK_NO_ERROR) return result;
    BridgeSetStage("display_part");
    result = ProMdlDisplay(model);
    if (result != PRO_TK_NO_ERROR) return result;
    if (ProMdlWindowGet(model, &window) == PRO_TK_NO_ERROR) {
        ProWindowCurrentSet(window);
        ProWindowActivate(window);
    }
    tracked_model = model;
    wcscpy_s(tracked_name, PRO_NAME_SIZE, name);
    return PRO_TK_NO_ERROR;
}

static ProError TrackedPart(ProName name, ProMdl *model)
{
    ProError result;
    BridgeSetStage("target_part_in_bridge_session");
    if (!tracked_model || _wcsicmp(name, tracked_name)) return ERR_OUTSIDE_SESSION;
    result = ProMdlInit(name, PRO_MDL_PART, model);
    if (result != PRO_TK_NO_ERROR) return result;
    if (*model != tracked_model) return ERR_OUTSIDE_SESSION;
    return ConfirmMetric(*model);
}

static ProError NewPart(ProName name)
{
    ProMdl template_part, new_part, existing;
    ProPath existing_path, previous_directory;
    ProError result;
    BridgeSetStage("new_part_name_available");
    if (FindSavedPart(name, existing_path) || ProMdlInit(name, PRO_MDL_PART, &existing) == PRO_TK_NO_ERROR)
        return PRO_TK_E_FOUND;
    BridgeSetStage("load_m250_metric_template");
    result = ProMdlLoad(template_path, PRO_MDL_PART, PRO_B_FALSE, &template_part);
    if (result != PRO_TK_NO_ERROR) return result;
    BridgeSetStage("select_part_creation_directory");
    result = ProDirectoryCurrentGet(previous_directory);
    if (result != PRO_TK_NO_ERROR) return result;
    if (_wcsicmp(previous_directory, model_path)) {
        result = ProDirectoryChange(model_path);
        if (result != PRO_TK_NO_ERROR) return result;
    }
    BridgeSetStage("copy_metric_template");
    result = ProMdlCopy(template_part, name, &new_part);
    if (_wcsicmp(previous_directory, model_path)) ProDirectoryChange(previous_directory);
    if (result != PRO_TK_NO_ERROR) return result;
    return DisplayTracked(new_part, name);
}

static ProError OpenPart(ProName name, int reload)
{
    ProPath path;
    ProMdldata source;
    ProMdl model;
    ProError result;
    ProBoolean modified;
    BridgeSetStage("find_saved_part_in_session_models");
    if (!FindSavedPart(name, path)) return PRO_TK_E_NOT_FOUND;
    if (ProMdlInit(name, PRO_MDL_PART, &model) == PRO_TK_NO_ERROR) {
        if (model != tracked_model || _wcsicmp(name, tracked_name)) {
            size_t length;
            BridgeSetStage("check_loaded_part_origin");
            result = ProMdlDataGet(model, &source);
            if (result != PRO_TK_NO_ERROR) return result;
            length = wcslen(source.path);
            while (length > 3 && (source.path[length - 1] == L'\\' || source.path[length - 1] == L'/'))
                source.path[--length] = L'\0';
            if (_wcsicmp(source.path, model_path)) return ERR_OUTSIDE_SESSION;
        }
        if (!reload) return DisplayTracked(model, name);
        BridgeSetStage("ensure_saved_before_reload");
        result = ProMdlModificationVerify(model, &modified);
        if (result != PRO_TK_NO_ERROR) return result;
        if (modified) return ERR_DIRTY_RELOAD;
        BridgeSetStage("erase_before_reload");
        result = ProMdlErase(model);
        if (result != PRO_TK_NO_ERROR) return result;
        tracked_model = NULL;
        tracked_name[0] = L'\0';
        wcscpy_s(reload_name, PRO_NAME_SIZE, name);
        return DEFER_RELOAD;
    }
    BridgeSetStage("load_saved_native_part");
    result = ProMdlLoad(path, PRO_MDL_PART, PRO_B_FALSE, &model);
    if (result != PRO_TK_NO_ERROR) return result;
    return DisplayTracked(model, name);
}

static ProError SavePart(ProMdl model)
{
    ProPath original;
    ProError result;
    int changed;
    BridgeSetStage("locate_model_save_directory");
    result = ProDirectoryCurrentGet(original);
    if (result != PRO_TK_NO_ERROR) return result;
    changed = _wcsicmp(original, model_path) != 0;
    if (changed) {
        result = ProDirectoryChange(model_path);
        if (result != PRO_TK_NO_ERROR) return result;
    }
    BridgeSetStage("save_native_part");
    result = ProMdlSave(model);
    if (changed) ProDirectoryChange(original);
    return result;
}

static const char *FeatureType(int type)
{
    if (type == PRO_FEAT_CURVE) return "sketch";
    if (type == PRO_FEAT_PROTRUSION || type == PRO_FEAT_FIRST_FEAT) return "extrusion";
    if (type == PRO_FEAT_HOLE) return "hole";
    if (type == PRO_FEAT_DATUM) return "datum_plane";
    return "other";
}

static int IsLengthDimension(ProDimension *dimension)
{
    ProDimensiontype type;
    if (ProDimensionTypeGet(dimension, &type) != PRO_TK_NO_ERROR) return 0;
    return type == PRODIMTYPE_LINEAR || type == PRODIMTYPE_RADIUS || type == PRODIMTYPE_DIAMETER;
}

static ProError ListDimension(ProDimension *dimension, ProError status, ProAppData data)
{
    int *count = (int *)data;
    ProName symbol = L"";
    ProBoolean driving = PRO_B_FALSE;
    double value;
    if (status != PRO_TK_NO_ERROR) return status;
    if (ProDimensionValueGet(dimension, &value) != PRO_TK_NO_ERROR || !_finite(value)) return PRO_TK_NO_ERROR;
    ProDimensionSymbolGet(dimension, symbol);
    ProDimensionIsDriving(dimension, &driving);
    ReplyAdd("%s{\"id\":%d,\"symbol\":", (*count)++ ? "," : "", dimension->id);
    ReplyWideString(symbol);
    ReplyAdd(",\"value\":%.17g,\"length_mm\":%s,\"driving\":%s}", value,
             IsLengthDimension(dimension) ? "true" : "false", driving ? "true" : "false");
    return response_full ? ERR_OUTPUT_LIMIT : PRO_TK_NO_ERROR;
}

static ProError ListFeature(ProFeature *feature, ProError status, ProAppData data)
{
    int *count = (int *)data;
    int dimensions = 0;
    ProFeattype type;
    ProFeatStatus feature_status;
    ProName name = L"";
    if (status != PRO_TK_NO_ERROR) return status;
    if (ProFeatureTypeGet(feature, &type) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (ProFeatureStatusGet(feature, &feature_status) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    ProModelitemNameGet(feature, name);
    ReplyAdd("%s{\"id\":%d,\"type\":\"%s\",\"type_code\":%d,\"status\":%d,\"name\":",
             (*count)++ ? "," : "", feature->id, FeatureType(type), (int)type, (int)feature_status);
    ReplyWideString(name);
    ReplyAdd(",\"dimensions\":[");
    status = ProFeatureDimensionVisit(feature, ListDimension, NULL, &dimensions);
    ReplyAdd("]}");
    return status == ERR_OUTPUT_LIMIT || response_full ? ERR_OUTPUT_LIMIT : PRO_TK_NO_ERROR;
}

static ProError InspectPart(ProMdl model)
{
    int count = 0;
    ProError result;
    ProMassProperty mass;
    ProBoolean modified;
    BridgeSetStage("inspect_native_feature_tree");
    if (ProMdlModificationVerify(model, &modified) == PRO_TK_NO_ERROR)
        ReplyAdd(",\"modified\":%s", modified ? "true" : "false");
    ReplyAdd(",\"features\":[");
    result = ProSolidFeatVisit((ProSolid)model, ListFeature, NULL, &count);
    if (result != PRO_TK_NO_ERROR && result != PRO_TK_E_NOT_FOUND) return result;
    ReplyAdd("]");
    result = ProSolidMassPropertyGet((ProSolid)model, NULL, &mass);
    if (result == PRO_TK_NO_ERROR && _finite(mass.volume) && _finite(mass.center_of_gravity[0]) &&
        _finite(mass.center_of_gravity[1]) && _finite(mass.center_of_gravity[2])) {
        ReplyAdd(",\"volume_mm3\":%.17g,\"centroid_mm\":[%.17g,%.17g,%.17g]", mass.volume,
                 mass.center_of_gravity[0], mass.center_of_gravity[1], mass.center_of_gravity[2]);
    } else ReplyAdd(",\"volume_mm3\":null,\"centroid_mm\":null");
    return PRO_TK_NO_ERROR;
}

struct DimensionLookup {
    int id;
    int found;
    ProDimension dimension;
};

static ProError FindDimension(ProDimension *dimension, ProError status, ProAppData data)
{
    struct DimensionLookup *lookup = (struct DimensionLookup *)data;
    if (status != PRO_TK_NO_ERROR) return status;
    if (dimension->id == lookup->id) {
        lookup->found = 1;
        lookup->dimension = *dimension;
    }
    return PRO_TK_NO_ERROR;
}

static ProError Regenerate(ProSolid solid)
{
    ProSolidRegenerationStatus status;
    ProError result;
    result = ProSolidRegenerate(solid, PRO_REGEN_NO_RESOLVE_MODE | PRO_REGEN_UNDO_IF_FAIL);
    if (result != PRO_TK_NO_ERROR) return result;
    result = ProSolidRegenerationstatusGet(solid, &status);
    if (result != PRO_TK_NO_ERROR) return result;
    if (status == PRO_SOLID_FAILED_REGENERATION || status == PRO_SOLID_CONNECT_FAILED) return PRO_TK_GENERAL_ERROR;
    return PRO_TK_NO_ERROR;
}

static ProError ChangeDimension(ProSolid solid, int feature_id, int dimension_id, double value)
{
    ProFeature feature;
    struct DimensionLookup lookup;
    ProBoolean driving;
    ProError result;
    double previous;
    BridgeSetStage("find_driving_dimension_on_feature");
    result = ProFeatureInit(solid, feature_id, &feature);
    if (result != PRO_TK_NO_ERROR) return result;
    lookup.id = dimension_id;
    lookup.found = 0;
    result = ProFeatureDimensionVisit(&feature, FindDimension, NULL, &lookup);
    if (result != PRO_TK_NO_ERROR) return result;
    if (!lookup.found) return PRO_TK_E_NOT_FOUND;
    if (!IsLengthDimension(&lookup.dimension)) return ERR_NON_LENGTH_DIMENSION;
    result = ProDimensionIsDriving(&lookup.dimension, &driving);
    if (result != PRO_TK_NO_ERROR) return result;
    if (!driving) return PRO_TK_BAD_INPUTS;
    result = ProDimensionValueGet(&lookup.dimension, &previous);
    if (result != PRO_TK_NO_ERROR) return result;
    BridgeSetStage("set_native_dimension");
    result = ProDimensionValueSet(&lookup.dimension, value);
    if (result != PRO_TK_NO_ERROR) return result;
    BridgeSetStage("regenerate_after_dimension_edit");
    result = Regenerate(solid);
    if (result != PRO_TK_NO_ERROR) {
        if (ProDimensionValueSet(&lookup.dimension, previous) != PRO_TK_NO_ERROR ||
            Regenerate(solid) != PRO_TK_NO_ERROR) {
            BridgeSetStage("dimension_edit_rollback_failed");
            return ERR_DIMENSION_RECOVERY;
        }
        return result;
    }
    ReplyAdd(",\"feature_id\":%d,\"dimension_id\":%d,\"previous_value\":%.17g,\"value\":%.17g",
             feature_id, dimension_id, previous, value);
    return PRO_TK_NO_ERROR;
}

static void PartReply(ProName name)
{
    ReplyAdd(",\"model\":");
    ReplyWideString(name);
}

static ProError Execute(char *request)
{
    char *fields[7];
    char *context;
    char *field;
    size_t count = 0;
    ProName name;
    ProMdl model;
    ProFeature feature;
    ProError result;
    double width, height, diameter;
    int reference_id, dimension_id;
    BridgeSetStage("validate_request");
    for (field = strtok_s(request, "\t\r\n", &context); field; field = strtok_s(NULL, "\t\r\n", &context)) {
        if (count == sizeof(fields) / sizeof(fields[0])) return PRO_TK_BAD_INPUTS;
        fields[count++] = field;
    }
    if (count == 1 && !strcmp(fields[0], "status")) {
        ReplyAdd(",\"pid\":%lu,\"protocol\":2", (unsigned long)GetCurrentProcessId());
        if (tracked_model) PartReply(tracked_name);
        return PRO_TK_NO_ERROR;
    }
    if (count < 2 || !ParseName(fields[1], name)) return PRO_TK_BAD_INPUTS;
    PartReply(name);
    if (!strcmp(fields[0], "new") && count == 2) return NewPart(name);
    if (!strcmp(fields[0], "open") && count == 3 && ParseInteger(fields[2], 0, &reference_id) && reference_id <= 1)
        return OpenPart(name, reference_id);
    result = TrackedPart(name, &model);
    if (result != PRO_TK_NO_ERROR) return result;
    if (!strcmp(fields[0], "inspect") && count == 2) return InspectPart(model);
    if (!strcmp(fields[0], "save") && count == 2) return SavePart(model);
    BridgeSetStage("validate_geometry_arguments");
    if (!strcmp(fields[0], "dimension") && count == 5 && ParseInteger(fields[2], 1, &reference_id) &&
        ParseInteger(fields[3], 0, &dimension_id) && ParseLength(fields[4], 0, &width))
        return ChangeDimension((ProSolid)model, reference_id, dimension_id, width);
    if (!strcmp(fields[0], "sketch") && count == 4 && ParseLength(fields[2], 0, &width) &&
        ParseLength(fields[3], 0, &height)) {
        result = BridgeSketchCreate((ProSolid)model, width, height, &feature);
    } else if (!strcmp(fields[0], "extrude") && count == 4 && ParseInteger(fields[2], 1, &reference_id) &&
               ParseLength(fields[3], 0, &width)) {
        result = BridgeExtrudeCreate((ProSolid)model, reference_id, width, &feature);
    } else if (!strcmp(fields[0], "hole") && count == 6 && ParseInteger(fields[2], 1, &reference_id) &&
               ParseLength(fields[3], 1, &width) && ParseLength(fields[4], 1, &height) &&
               ParseLength(fields[5], 0, &diameter)) {
        result = BridgeHoleCreate((ProSolid)model, reference_id, width, height, diameter, &feature);
    } else return PRO_TK_BAD_INPUTS;
    if (result == PRO_TK_NO_ERROR) {
        ProFeattype type;
        ReplyAdd(",\"feature_id\":%d", feature.id);
        if (ProFeatureTypeGet(&feature, &type) == PRO_TK_NO_ERROR) ReplyAdd(",\"type\":\"%s\"", FeatureType(type));
    }
    return result;
}

static void WriteResponse(const wchar_t *path, ProError result)
{
    if (response_full) {
        result = ERR_OUTPUT_LIMIT;
        BridgeSetStage("feature_tree_exceeds_response_limit");
    }
    if (result == PRO_TK_NO_ERROR) ReplyAdd("}\n");
    if (response_full || result != PRO_TK_NO_ERROR) {
        ReplyReset();
        ReplyAdd("{\"ok\":false,\"code\":%d,\"stage\":\"%s\"}\n", (int)result, current_stage);
    }
    WriteAtomic(path);
}

static int ValidRequestName(const wchar_t *name)
{
    size_t index;
    if (wcslen(name) != 36 || wcscmp(name + 32, L".req")) return 0;
    for (index = 0; index < 32; ++index) {
        if ((name[index] < L'0' || name[index] > L'9') &&
            (name[index] < L'a' || name[index] > L'f')) return 0;
    }
    return 1;
}

static unsigned __int64 CurrentUnixMilliseconds(void)
{
    FILETIME current;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&current);
    ticks.LowPart = current.dwLowDateTime;
    ticks.HighPart = current.dwHighDateTime;
    return (ticks.QuadPart - 116444736000000000ULL) / 10000ULL;
}

static ProError ProcessSessionRequest(char *request)
{
    char *field_end, *digit;
    unsigned __int64 deadline, now;
    BridgeSetStage("request_expired_or_belongs_to_another_creo_session");
    if (strlen(request) < 47 || memcmp(request, session_epoch, 32) != 0 || request[32] != '\t')
        return ERR_STALE_REQUEST;
    field_end = strchr(request + 33, '\t');
    if (field_end == NULL || field_end - (request + 33) != 13) return ERR_STALE_REQUEST;
    for (digit = request + 33; digit < field_end; ++digit) {
        if (*digit < '0' || *digit > '9') return ERR_STALE_REQUEST;
    }
    deadline = _strtoui64(request + 33, NULL, 10);
    now = CurrentUnixMilliseconds();
    if (deadline < now || deadline - now > 60000ULL) return ERR_STALE_REQUEST;
    return Execute(field_end + 1);
}

static void ProcessQueue(void)
{
    ProPath pattern, request_path, claimed_path, reply_path;
    wchar_t file_name[40];
    WIN32_FIND_DATAW entry;
    HANDLE search;
    FILE *stream;
    char request[REQUEST_SIZE + 1];
    size_t count;
    ProError result;
    if (reload_reply[0]) {
        ProMdl erased;
        ReplyReset();
        ReplyAdd("{\"ok\":true");
        PartReply(reload_name);
        BridgeSetStage("verify_model_erased_before_loading_from_disk");
        result = ProMdlInit(reload_name, PRO_MDL_PART, &erased) == PRO_TK_NO_ERROR
            ? PRO_TK_E_FOUND : OpenPart(reload_name, 0);
        WriteResponse(reload_reply, result);
        reload_reply[0] = L'\0';
        return;
    }
    if (!JoinPath(pattern, queue_path, L"*.req")) return;
    search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !ValidRequestName(entry.cFileName)) continue;
        if (!JoinPath(request_path, queue_path, entry.cFileName)) continue;
        wcscpy_s(file_name, sizeof(file_name) / sizeof(file_name[0]), entry.cFileName);
        wcscpy_s(file_name + 32, 8, L".run");
        if (!JoinPath(claimed_path, queue_path, file_name)) continue;
        wcscpy_s(file_name + 32, 8, L".res");
        if (!JoinPath(reply_path, queue_path, file_name)) continue;
        if (!MoveFileExW(request_path, claimed_path, 0)) continue;
        BridgeSetStage("read_claimed_request");
        ReplyReset();
        ReplyAdd("{\"ok\":true");
        count = 0;
        if (_wfopen_s(&stream, claimed_path, L"rb") == 0) {
            count = fread(request, 1, REQUEST_SIZE, stream);
            fclose(stream);
        }
        request[count] = '\0';
        result = count > 0 && count < REQUEST_SIZE && strlen(request) == count
            ? ProcessSessionRequest(request) : PRO_TK_BAD_INPUTS;
        DeleteFileW(claimed_path);
        if (result == DEFER_RELOAD) wcscpy_s(reload_reply, PRO_PATH_SIZE, reply_path);
        else WriteResponse(reply_path, result);
        break;
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

static VOID CALLBACK PollQueue(HWND window, UINT message, UINT_PTR id, DWORD elapsed)
{
    HWND active;
    wchar_t window_class[20];
    (void)window;
    (void)message;
    (void)id;
    (void)elapsed;
    if (busy) return;
    active = GetActiveWindow();
    if (active && GetClassNameW(active, window_class, 20) && !wcscmp(window_class, L"#32770")) return;
    busy = 1;
    ProcessQueue();
    busy = 0;
}

__declspec(dllexport) int user_initialize(int argc, char **argv, char *version,
                                          char *build, wchar_t error[80])
{
    DWORD size;
    BYTE epoch_bytes[16];
    HCRYPTPROV provider = 0;
    int index;
    int creo_wchar_size;
    (void)argc;
    (void)argv;
    (void)version;
    (void)build;
    if (ProWcharSizeVerify((int)sizeof(wchar_t), &creo_wchar_size) != PRO_TK_NO_ERROR) {
        wcscpy_s(error, 80, L"Creo and the MCP bridge disagree on wchar_t size.");
        return PRO_TK_GENERAL_ERROR;
    }
    size = GetEnvironmentVariableW(L"CREO2_BRIDGE_SESSION", session_path, PRO_PATH_SIZE);
    if (size == 0 || size >= PRO_PATH_SIZE || !JoinPath(queue_path, session_path, L"queue") ||
        !JoinPath(model_path, session_path, L"models") || !JoinPath(ready_path, session_path, L"ready.json")) {
        wcscpy_s(error, 80, L"Start Creo using the Creo 2.0 MCP launcher.");
        return PRO_TK_BAD_INPUTS;
    }
    size = GetEnvironmentVariableW(L"CREO2_BRIDGE_TEMPLATE", template_path, PRO_PATH_SIZE);
    if (size == 0 || size >= PRO_PATH_SIZE || GetFileAttributesW(template_path) == INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(error, 80, L"Creo 2.0 metric part template is missing.");
        return PRO_TK_E_NOT_FOUND;
    }
    wire_locale = _create_locale(LC_NUMERIC, "C");
    if (!wire_locale) return PRO_TK_GENERAL_ERROR;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT) ||
        !CryptGenRandom(provider, (DWORD)sizeof(epoch_bytes), epoch_bytes)) {
        if (provider) CryptReleaseContext(provider, 0);
        _free_locale(wire_locale);
        wire_locale = NULL;
        wcscpy_s(error, 80, L"Cannot initialize a fresh Creo MCP session identifier.");
        return PRO_TK_GENERAL_ERROR;
    }
    CryptReleaseContext(provider, 0);
    for (index = 0; index < (int)sizeof(epoch_bytes); ++index)
        sprintf_s(session_epoch + index * 2, 3, "%02x", (unsigned)epoch_bytes[index]);
    timer_id = SetTimer(NULL, 0, 220, PollQueue);
    ReplyReset();
    ReplyAdd("{\"pid\":%lu,\"protocol\":2,\"epoch\":\"%s\"}\n",
             (unsigned long)GetCurrentProcessId(), session_epoch);
    if (!timer_id || !WriteAtomic(ready_path)) {
        if (timer_id) KillTimer(NULL, timer_id);
        timer_id = 0;
        _free_locale(wire_locale);
        wire_locale = NULL;
        wcscpy_s(error, 80, L"Cannot open the Creo 2.0 MCP session folder.");
        return PRO_TK_GENERAL_ERROR;
    }
    return PRO_TK_NO_ERROR;
}

__declspec(dllexport) void user_terminate(void)
{
    if (timer_id) KillTimer(NULL, timer_id);
    if (ready_path[0]) DeleteFileW(ready_path);
    if (wire_locale) _free_locale(wire_locale);
}
