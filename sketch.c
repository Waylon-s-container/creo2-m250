#include <float.h>
#include <math.h>
#include <stdio.h>

#include "bridge.h"

#include <ProArray.h>
#include <ProDimension.h>
#include <ProDtmCrv.h>
#include <ProElemId.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProExtrude.h>
#include <ProFeatForm.h>
#include <ProFeatType.h>
#include <ProGeomitemdata.h>
#include <ProMdl.h>
#include <ProModelitem.h>
#include <ProReference.h>
#include <ProSecdim.h>
#include <ProSection.h>
#include <ProSolid.h>
#include <ProStdSection.h>
#include <ProUtil.h>

#define TRY(call) if ((status = (call)) != PRO_TK_NO_ERROR) goto cleanup

typedef struct OriginDatums {
    ProModelitem planes[3];
} OriginDatums;

typedef struct ExtrudeCap {
    double depth;
    int positive;
    int negative;
} ExtrudeCap;

static ProError add_compound(ProElement parent, ProElemId id, ProElement *child)
{
    ProError status = ProElementAlloc(id, child);
    if (status == PRO_TK_NO_ERROR) {
        status = ProElemtreeElementAdd(parent, NULL, *child);
        if (status != PRO_TK_NO_ERROR) ProElementFree(child);
    }
    return status;
}

static ProError add_integer(ProElement parent, ProElemId id, int value)
{
    ProError status;
    ProElement child = NULL;

    TRY(ProElementAlloc(id, &child));
    TRY(ProElementIntegerSet(child, value));
    TRY(ProElemtreeElementAdd(parent, NULL, child));
    return PRO_TK_NO_ERROR;

cleanup:
    if (child != NULL) ProElementFree(&child);
    return status;
}

static ProError add_reference(ProElement parent, ProElemId id, ProSelection selection)
{
    ProError status;
    ProElement child = NULL;
    ProReference reference = NULL;

    TRY(ProSelectionToReference(selection, &reference));
    TRY(ProElementAlloc(id, &child));
    TRY(ProElementReferenceSet(child, reference));
    TRY(ProElemtreeElementAdd(parent, NULL, child));
    child = NULL;

cleanup:
    if (child != NULL) ProElementFree(&child);
    if (reference != NULL) ProReferenceFree(reference);
    return status;
}

static ProError make_placement(ProSolid solid, ProSelection *selection)
{
    ProModelitem model_item;
    ProError status = ProMdlToModelitem((ProMdl)solid, &model_item);
    if (status != PRO_TK_NO_ERROR) return status;
    return ProSelectionAlloc(NULL, &model_item, selection);
}

static ProError check_active(ProFeature *feature)
{
    ProBoolean incomplete;
    ProFeatStatus feature_status;
    unsigned int flags;
    ProError status;

    TRY(ProFeatureIsIncomplete(feature, &incomplete));
    TRY(ProFeatureStatusGet(feature, &feature_status));
    TRY(ProFeatureStatusflagsGet(feature, &flags));
    if (incomplete == PRO_B_TRUE || feature_status != PRO_FEAT_ACTIVE ||
        (flags & (PRO_FEAT_STAT_FAILED | PRO_FEAT_STAT_UNREGENERATED |
                  PRO_FEAT_STAT_CHILD_OF_FAILED | PRO_FEAT_STAT_CHILD_OF_EXT_FAILED))) {
        return PRO_TK_GENERAL_ERROR;
    }

cleanup:
    return status;
}

static ProError remove_failed_feature(ProSolid solid, int id)
{
    ProFeatureDeleteOptions options[] = { PRO_FEAT_DELETE_NO_OPTS };
    return ProFeatureDelete(solid, &id, 1, options, 1);
}

static void set_dimension_label(ProDimension *dimension, const char *prefix, int feature_id)
{
    ProCharName ascii;
    ProName symbol;

    if (sprintf_s(ascii, sizeof(ascii), "%s_%d", prefix, feature_id) >= 0) {
        ProStringToWstring(symbol, ascii);
        ProDimensionSymbolSet(dimension, symbol);
    }
}

static ProError plane_get(ProGeomitem *item, ProPlanedata *plane)
{
    ProGeomitemdata *data = NULL;
    ProError status = ProGeomitemdataGet(item, &data);

    if (status == PRO_TK_NO_ERROR) {
        if (data != NULL && data->data.p_surface_data != NULL &&
            data->data.p_surface_data->type == PRO_SRF_PLANE) {
            *plane = data->data.p_surface_data->srf_shape.plane;
        } else {
            status = PRO_TK_INVALID_TYPE;
        }
    }
    if (data != NULL) ProGeomitemdataFree(&data);
    return status;
}

static ProError find_datum_plane(ProGeomitem *item, ProError status, ProAppData data)
{
    OriginDatums *datums = (OriginDatums *)data;
    ProPlanedata plane;
    double distance;
    int axis;

    if (status != PRO_TK_NO_ERROR || plane_get(item, &plane) != PRO_TK_NO_ERROR) {
        return PRO_TK_NO_ERROR;
    }
    distance = plane.origin[0] * plane.e3[0] + plane.origin[1] * plane.e3[1] +
               plane.origin[2] * plane.e3[2];
    if (fabs(distance) > 1e-6) return PRO_TK_NO_ERROR;

    for (axis = 0; axis < 3; ++axis) {
        if (fabs(plane.e3[axis]) > 1.0 - 1e-9 && datums->planes[axis].owner == NULL) {
            datums->planes[axis] = *item;
        }
    }
    return PRO_TK_NO_ERROR;
}

static ProError find_datum_feature(ProFeature *feature, ProError status, ProAppData data)
{
    ProFeattype type;
    ProFeatStatus feature_status;

    if (status != PRO_TK_NO_ERROR) return status;
    if (ProFeatureTypeGet(feature, &type) != PRO_TK_NO_ERROR || type != PRO_FEAT_DATUM ||
        ProFeatureStatusGet(feature, &feature_status) != PRO_TK_NO_ERROR ||
        feature_status != PRO_FEAT_ACTIVE) {
        return PRO_TK_NO_ERROR;
    }
    status = ProFeatureGeomitemVisit(feature, PRO_SURFACE, find_datum_plane, NULL, data);
    return status == PRO_TK_E_NOT_FOUND ? PRO_TK_NO_ERROR : status;
}

static ProError get_sketcher(ProElement tree, ProSection *section)
{
    ProElempath path = NULL;
    ProElempathItem items[2];
    ProElement sketcher;
    ProAppData value = NULL;
    ProError status;

    items[0].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    items[0].path_item.elem_id = PRO_E_STD_SECTION;
    items[1].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    items[1].path_item.elem_id = PRO_E_SKETCHER;
    TRY(ProElempathAlloc(&path));
    TRY(ProElempathDataSet(path, items, 2));
    TRY(ProElemtreeElementGet(tree, path, &sketcher));
    TRY(ProElementSpecialvalueGet(sketcher, NULL, &value));
    if (value == NULL) status = PRO_TK_GENERAL_ERROR;
    *section = (ProSection)value;

cleanup:
    if (path != NULL) ProElempathFree(&path);
    return status;
}

static void to_sketch(ProMatrix location, double model_x, double model_y, Pro2dPnt point)
{
    double delta_x = model_x - location[3][0];
    double delta_y = model_y - location[3][1];
    double delta_z = -location[3][2];

    point[0] = delta_x * location[0][0] + delta_y * location[0][1] + delta_z * location[0][2];
    point[1] = delta_x * location[1][0] + delta_y * location[1][1] + delta_z * location[1][2];
}

static int near_point(const double *first, const double *second, double tolerance)
{
    return fabs(first[0] - second[0]) <= tolerance && fabs(first[1] - second[1]) <= tolerance;
}

static ProError build_rectangle(ProSection section, ProSelection *axes,
                                double width, double height, int *size_dims)
{
    ProError status;
    ProWSecerror errors = NULL;
    ProMatrix location;
    ProSectionShape shape;
    Pro2dLinedef edge;
    Pro2dLinedef *solved_edge;
    Pro2dEntdef *entity;
    Pro2dPnt corners[4], label;
    ProSectionPointType whole = PRO_ENT_WHOLE;
    double tolerance = (width > height ? width : height) * 1e-8 + 1e-5;
    int ids[4], projection, edge_index, next;

    TRY(ProSectionLocationGet(section, location));
    TRY(ProSectionEpsilonSet(section, 1e-9));
    TRY(ProSectionEntityFromProjection(section, axes[0], &projection));
    TRY(ProSectionEntityFromProjection(section, axes[1], &projection));
    to_sketch(location, -width / 2.0, -height / 2.0, corners[0]);
    to_sketch(location,  width / 2.0, -height / 2.0, corners[1]);
    to_sketch(location,  width / 2.0,  height / 2.0, corners[2]);
    to_sketch(location, -width / 2.0,  height / 2.0, corners[3]);

    edge.type = PRO_2D_LINE;
    for (edge_index = 0; edge_index < 4; ++edge_index) {
        next = (edge_index + 1) % 4;
        edge.end1[0] = corners[edge_index][0];
        edge.end1[1] = corners[edge_index][1];
        edge.end2[0] = corners[next][0];
        edge.end2[1] = corners[next][1];
        TRY(ProSectionEntityAdd(section, (Pro2dEntdef *)&edge, &ids[edge_index]));
    }

    to_sketch(location, 0.0, -height * 0.7, label);
    TRY(ProSecdimCreate(section, &ids[0], &whole, 1, PRO_TK_DIM_LINE, label, &size_dims[0]));
    to_sketch(location, width * 0.7, 0.0, label);
    TRY(ProSecdimCreate(section, &ids[1], &whole, 1, PRO_TK_DIM_LINE, label, &size_dims[1]));

    BridgeSetStage("solve rectangular sketch constraints");
    TRY(ProSecerrorAlloc(&errors));
    TRY(ProSectionAutodim(section, &errors));
    TRY(ProSecdimValueSet(section, size_dims[0], width));
    TRY(ProSecdimValueSet(section, size_dims[1], height));
    TRY(ProSectionRegenerate(section, &errors));
    TRY(ProSectionShapeGet(section, &shape));
    if (shape != PRO_SECSHAPE_1_CLOSED_LOOP) {
        status = PRO_TK_GENERAL_ERROR;
        goto cleanup;
    }

    BridgeSetStage("verify the regenerated rectangle dimensions and placement");
    for (edge_index = 0; edge_index < 4; ++edge_index) {
        next = (edge_index + 1) % 4;
        TRY(ProSectionEntityGet(section, ids[edge_index], &entity));
        if (entity->type != PRO_2D_LINE) {
            status = PRO_TK_INVALID_TYPE;
            goto cleanup;
        }
        solved_edge = (Pro2dLinedef *)entity;
        if (!(near_point(solved_edge->end1, corners[edge_index], tolerance) &&
              near_point(solved_edge->end2, corners[next], tolerance)) &&
            !(near_point(solved_edge->end2, corners[edge_index], tolerance) &&
              near_point(solved_edge->end1, corners[next], tolerance))) {
            status = PRO_TK_GENERAL_ERROR;
            goto cleanup;
        }
    }

cleanup:
    if (errors != NULL) ProSecerrorFree(&errors);
    return status;
}

static void label_size_dimensions(ProSolid solid, ProFeature *feature, int *size_dims)
{
    const char *prefixes[2] = { "RECT_WIDTH", "RECT_HEIGHT" };
    ProSection copy = NULL;
    ProIntlist section_ids = NULL;
    ProIntlist solid_ids = NULL;
    ProDimension dimension;
    int count, index, which;

    if (ProFeatureSectionCopy(feature, 0, &copy) != PRO_TK_NO_ERROR) return;
    if (ProSectionDimensionIdsGet(copy, &section_ids, &solid_ids, &count) == PRO_TK_NO_ERROR) {
        for (index = 0; index < count; ++index) {
            for (which = 0; which < 2; ++which) {
                if (section_ids[index] == size_dims[which] &&
                    ProModelitemInit((ProMdl)solid, solid_ids[index], PRO_DIMENSION,
                                     &dimension) == PRO_TK_NO_ERROR) {
                    set_dimension_label(&dimension, prefixes[which], feature->id);
                }
            }
        }
    }
    if (section_ids != NULL) ProArrayFree((ProArray *)&section_ids);
    if (solid_ids != NULL) ProArrayFree((ProArray *)&solid_ids);
    ProSectionFree(&copy);
}

ProError BridgeSketchCreate(ProSolid solid, double width_mm, double height_mm, ProFeature *feature)
{
    OriginDatums datums = { 0 };
    ProFeature created = { 0 };
    ProError status, rollback_status;
    ProElement tree = NULL, section_tree, setup, extracted = NULL;
    ProSelection placement = NULL, planes[3] = { NULL, NULL, NULL };
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = { 0 };
    ProSection section = NULL;
    int axis, size_dims[2];

    if (solid == NULL || feature == NULL || !_finite(width_mm) || !_finite(height_mm) ||
        width_mm <= 0.0 || height_mm <= 0.0) return PRO_TK_BAD_INPUTS;

    BridgeSetStage("find the three origin datum planes");
    TRY(ProSolidFeatVisit(solid, find_datum_feature, NULL, &datums));
    for (axis = 0; axis < 3; ++axis) {
        if (datums.planes[axis].owner == NULL) {
            status = PRO_TK_E_NOT_FOUND;
            goto cleanup;
        }
        TRY(ProSelectionAlloc(NULL, &datums.planes[axis], &planes[axis]));
    }

    BridgeSetStage("define the native sketch plane");
    TRY(ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    TRY(add_integer(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CURVE));
    TRY(add_integer(tree, PRO_E_CURVE_TYPE, PRO_CURVE_TYPE_SKETCHED));
    TRY(add_compound(tree, PRO_E_STD_SECTION, &section_tree));
    TRY(add_compound(section_tree, PRO_E_STD_SEC_SETUP_PLANE, &setup));
    TRY(add_reference(setup, PRO_E_STD_SEC_PLANE, planes[2]));
    TRY(add_integer(setup, PRO_E_STD_SEC_PLANE_VIEW_DIR, PRO_SEC_VIEW_DIR_SIDE_ONE));
    TRY(add_integer(setup, PRO_E_STD_SEC_PLANE_ORIENT_DIR, PRO_SEC_ORIENT_DIR_UP));
    TRY(add_reference(setup, PRO_E_STD_SEC_PLANE_ORIENT_REF, planes[1]));
    TRY(make_placement(solid, &placement));
    TRY(ProArrayAlloc(1, sizeof(*options), 1, (ProArray *)&options));

    BridgeSetStage("create the standalone sketch feature");
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    TRY(ProFeatureWithoptionsCreate(placement, tree, options, PRO_REGEN_NO_FLAGS, &created, &errors));
    TRY(ProFeatureElemtreeExtract(&created, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &extracted));
    TRY(get_sketcher(extracted, &section));

    BridgeSetStage("draw the rectangle and assign driving dimensions");
    TRY(build_rectangle(section, planes, width_mm, height_mm, size_dims));

    BridgeSetStage("regenerate the standalone sketch feature");
    options[0] = PRO_FEAT_CR_NO_OPTS;
    TRY(ProFeatureWithoptionsRedefine(NULL, &created, extracted, options, PRO_REGEN_NO_FLAGS, &errors));
    TRY(check_active(&created));
    label_size_dimensions(solid, &created, size_dims);
    *feature = created;

cleanup:
    if (extracted != NULL) ProFeatureElemtreeFree(&created, extracted);
    if (tree != NULL) ProElementFree(&tree);
    for (axis = 0; axis < 3; ++axis) {
        if (planes[axis] != NULL) ProSelectionFree(&planes[axis]);
    }
    if (placement != NULL) ProSelectionFree(&placement);
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (status != PRO_TK_NO_ERROR && created.id > 0) {
        rollback_status = remove_failed_feature(solid, created.id);
        if (rollback_status != PRO_TK_NO_ERROR) {
            BridgeSetStage("remove the incomplete sketch after failure");
            return rollback_status;
        }
    }
    return status;
}

static ProError visit_cap(ProGeomitem *item, ProError status, ProAppData data)
{
    ExtrudeCap *cap = (ExtrudeCap *)data;
    ProPlanedata plane;
    double tolerance = cap->depth * 1e-6 + 1e-6;

    if (status == PRO_TK_NO_ERROR && plane_get(item, &plane) == PRO_TK_NO_ERROR &&
        fabs(plane.e3[2]) > 1.0 - 1e-9) {
        if (fabs(plane.origin[2] - cap->depth) <= tolerance) cap->positive = 1;
        if (fabs(plane.origin[2] + cap->depth) <= tolerance) cap->negative = 1;
    }
    return PRO_TK_NO_ERROR;
}

static void label_depth_dimension(ProSolid solid, ProFeature *feature)
{
    ProElempath path = NULL;
    ProElempathItem items[3];
    ProDimension dimension;
    int id;

    items[0].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    items[0].path_item.elem_id = PRO_E_STD_EXT_DEPTH;
    items[1].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    items[1].path_item.elem_id = PRO_E_EXT_DEPTH_TO;
    items[2].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    items[2].path_item.elem_id = PRO_E_EXT_DEPTH_TO_VALUE;
    if (ProElempathAlloc(&path) != PRO_TK_NO_ERROR) return;
    if (ProElempathDataSet(path, items, 3) == PRO_TK_NO_ERROR &&
        ProFeatureElemDimensionIdGet(feature, path, &id) == PRO_TK_NO_ERROR &&
        ProModelitemInit((ProMdl)solid, id, PRO_DIMENSION, &dimension) == PRO_TK_NO_ERROR) {
        set_dimension_label(&dimension, "EXTRUDE_DEPTH", feature->id);
    }
    ProElempathFree(&path);
}

static ProError reverse_extrusion(ProFeature *feature, ProFeatureCreateOptions *options)
{
    ProElement tree = NULL, direction;
    ProElempath path = NULL;
    ProElempathItem item;
    ProErrorlist errors = { 0 };
    ProError status;
    int side;

    item.type = PRO_ELEM_PATH_ITEM_TYPE_ID;
    item.path_item.elem_id = PRO_E_STD_DIRECTION;
    TRY(ProFeatureElemtreeExtract(feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &tree));
    TRY(ProElempathAlloc(&path));
    TRY(ProElempathDataSet(path, &item, 1));
    TRY(ProElemtreeElementGet(tree, path, &direction));
    TRY(ProElementIntegerGet(direction, NULL, &side));
    TRY(ProElementIntegerSet(direction, -side));
    TRY(ProFeatureWithoptionsRedefine(NULL, feature, tree, options, PRO_REGEN_NO_FLAGS, &errors));
    TRY(check_active(feature));

cleanup:
    if (path != NULL) ProElempathFree(&path);
    if (tree != NULL) ProFeatureElemtreeFree(feature, tree);
    return status;
}

static ProError verify_sketch_parent(ProFeature *extrusion, int sketch_id)
{
    int *parents = NULL;
    int count, index;
    ProError status;

    TRY(ProFeatureParentsGet(extrusion, &parents, &count));
    status = PRO_TK_E_NOT_FOUND;
    for (index = 0; index < count; ++index) {
        if (parents[index] == sketch_id) {
            status = PRO_TK_NO_ERROR;
            break;
        }
    }

cleanup:
    if (parents != NULL) ProArrayFree((ProArray *)&parents);
    return status;
}

ProError BridgeExtrudeCreate(ProSolid solid, int sketch_id, double depth_mm, ProFeature *feature)
{
    ProFeature sketch, created = { 0 };
    ProFeattype sketch_type;
    ProError status, rollback_status;
    ProSection sketch_section = NULL;
    ProSectionShape shape;
    ProMatrix location;
    ProElement tree = NULL, section_tree, depth, from, to, depth_value = NULL, draft;
    ProSelection placement = NULL, sketch_selection = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = { 0 };
    ExtrudeCap cap = { 0 };
    int side;

    if (solid == NULL || feature == NULL || sketch_id <= 0 ||
        !_finite(depth_mm) || depth_mm <= 0.0) return PRO_TK_BAD_INPUTS;

    BridgeSetStage("validate the closed planar sketch for the extrusion");
    TRY(ProFeatureInit(solid, sketch_id, &sketch));
    TRY(ProFeatureTypeGet(&sketch, &sketch_type));
    if (sketch_type != PRO_FEAT_CURVE) return PRO_TK_INVALID_TYPE;
    TRY(check_active(&sketch));
    TRY(ProFeatureSectionCopy(&sketch, 0, &sketch_section));
    TRY(ProSectionLocationGet(sketch_section, location));
    TRY(ProSectionShapeGet(sketch_section, &shape));
    if (shape != PRO_SECSHAPE_1_CLOSED_LOOP || fabs(location[2][2]) < 1.0 - 1e-9 ||
        fabs(location[3][2]) > 1e-6) {
        status = PRO_TK_BAD_INPUTS;
        goto cleanup;
    }
    side = location[2][2] > 0 ? PRO_EXT_CR_IN_SIDE_ONE : PRO_EXT_CR_IN_SIDE_TWO;

    BridgeSetStage("define the native extrusion and link the external sketch");
    TRY(ProSelectionAlloc(NULL, &sketch, &sketch_selection));
    TRY(ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    TRY(add_integer(tree, PRO_E_FEATURE_FORM, PRO_EXTRUDE));
    TRY(add_integer(tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_EXT_FEAT_TYPE_SOLID));
    TRY(add_integer(tree, PRO_E_REMOVE_MATERIAL, PRO_EXT_MATERIAL_ADD));
    TRY(add_integer(tree, PRO_E_FEAT_FORM_IS_THIN, PRO_EXT_FEAT_FORM_NO_THIN));
    TRY(add_compound(tree, PRO_E_STD_SECTION, &section_tree));
    TRY(add_reference(section_tree, PRO_E_SEC_USE_SKETCH, sketch_selection));
    TRY(add_integer(tree, PRO_E_STD_DIRECTION, side));
    TRY(add_compound(tree, PRO_E_STD_EXT_DEPTH, &depth));
    TRY(add_compound(depth, PRO_E_EXT_DEPTH_FROM, &from));
    TRY(add_integer(from, PRO_E_EXT_DEPTH_FROM_TYPE, PRO_EXT_DEPTH_FROM_NONE));
    TRY(add_compound(depth, PRO_E_EXT_DEPTH_TO, &to));
    TRY(add_integer(to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_TO_BLIND));
    TRY(ProElementAlloc(PRO_E_EXT_DEPTH_TO_VALUE, &depth_value));
    TRY(ProElementDecimalsSet(depth_value, 8));
    TRY(ProElementDoubleSet(depth_value, depth_mm));
    TRY(ProElemtreeElementAdd(to, NULL, depth_value));
    depth_value = NULL;
    TRY(add_compound(tree, PRO_E_EXT_COMP_DRFT_ANG, &draft));
    TRY(add_integer(draft, PRO_E_EXT_DRFT_ANG, PRO_EXT_DRFT_ANG_NO_DRAFT));
    TRY(make_placement(solid, &placement));
    TRY(ProArrayAlloc(1, sizeof(*options), 1, (ProArray *)&options));

    BridgeSetStage("create the linked blind solid extrusion");
    options[0] = PRO_FEAT_CR_NO_OPTS;
    TRY(ProFeatureWithoptionsCreate(placement, tree, options, PRO_REGEN_NO_FLAGS, &created, &errors));
    TRY(check_active(&created));

    BridgeSetStage("verify the extrusion ends at positive Z");
    cap.depth = depth_mm;
    TRY(ProFeatureGeomitemVisit(&created, PRO_SURFACE, visit_cap, NULL, &cap));
    if (!cap.positive && cap.negative) {
        TRY(reverse_extrusion(&created, options));
        TRY(ProFeatureGeomitemVisit(&created, PRO_SURFACE, visit_cap, NULL, &cap));
    }
    if (!cap.positive) {
        status = PRO_TK_E_NOT_FOUND;
        goto cleanup;
    }

    BridgeSetStage("verify the extrusion still links to the standalone sketch");
    TRY(verify_sketch_parent(&created, sketch_id));
    label_depth_dimension(solid, &created);
    *feature = created;

cleanup:
    if (depth_value != NULL) ProElementFree(&depth_value);
    if (tree != NULL) ProElementFree(&tree);
    if (placement != NULL) ProSelectionFree(&placement);
    if (sketch_selection != NULL) ProSelectionFree(&sketch_selection);
    if (sketch_section != NULL) ProSectionFree(&sketch_section);
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (status != PRO_TK_NO_ERROR && created.id > 0) {
        rollback_status = remove_failed_feature(solid, created.id);
        if (rollback_status != PRO_TK_NO_ERROR) {
            BridgeSetStage("remove the failed solid extrusion");
            return rollback_status;
        }
    }
    return status;
}
