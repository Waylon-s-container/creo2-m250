#include "bridge.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <ProArray.h>
#include <ProContour.h>
#include <ProEdge.h>
#include <ProElement.h>
#include <ProFeatType.h>
#include <ProGeomitem.h>
#include <ProHole.h>
#include <ProReference.h>
#include <ProSelection.h>
#include <ProSolid.h>
#include <ProSurface.h>

#define PLATE_EDGE_TOLERANCE 1.0e-7

typedef struct {
    ProSolid solid;
    ProSurface surface;
    ProGeomitem surface_item;
    ProPoint3d center;
    ProUvParam uv;
    ProEdge lower_edges[2];
    double lower[2];
    double upper[2];
} PlateHolePlacement;

static ProError FindTopFace(ProGeomitem *item, ProError status, ProAppData data)
{
    PlateHolePlacement *placement = (PlateHolePlacement *)data;
    ProSurface surface;
    ProSrftype surface_type;
    ProUvParam parameter_origin = {0.0, 0.0};
    ProPoint3d surface_origin;
    ProVector normal;

    if (status != PRO_TK_NO_ERROR) return status;
    status = ProGeomitemToSurface(item, &surface);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_PLANE) return status;
    status = ProSurfaceXyzdataEval(surface, parameter_origin, surface_origin,
                                   NULL, NULL, normal);
    if (status != PRO_TK_NO_ERROR) return status;
    if (normal[2] < 1.0 - PLATE_EDGE_TOLERANCE) return PRO_TK_NO_ERROR;
    if (placement->surface == NULL || surface_origin[2] > placement->center[2]) {
        placement->surface = surface;
        placement->surface_item = *item;
        placement->center[2] = surface_origin[2];
    }
    return PRO_TK_NO_ERROR;
}

static ProError FindBoundaryEdges(ProEdge edge, ProError status, ProAppData data)
{
    PlateHolePlacement *placement = (PlateHolePlacement *)data;
    ProEnttype edge_type;
    ProPoint3d start;
    ProPoint3d end;
    int axis;

    if (status != PRO_TK_NO_ERROR) return status;
    status = ProEdgeTypeGet(edge, &edge_type);
    if (status != PRO_TK_NO_ERROR || edge_type != PRO_ENT_LINE) return status;
    status = ProEdgeXyzdataEval(edge, 0.0, start, NULL, NULL, NULL);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProEdgeXyzdataEval(edge, 1.0, end, NULL, NULL, NULL);
    if (status != PRO_TK_NO_ERROR) return status;

    for (axis = 0; axis < 2; ++axis) {
        if (fabs(end[axis] - start[axis]) > PLATE_EDGE_TOLERANCE) continue;
        if (start[axis] < placement->lower[axis]) {
            placement->lower[axis] = start[axis];
            placement->lower_edges[axis] = edge;
        }
        if (start[axis] > placement->upper[axis]) placement->upper[axis] = start[axis];
    }
    return PRO_TK_NO_ERROR;
}

static ProError VisitOuterContour(ProContour contour, ProError status, ProAppData data)
{
    PlateHolePlacement *placement = (PlateHolePlacement *)data;
    ProContourTraversal traversal;

    if (status != PRO_TK_NO_ERROR) return status;
    status = ProContourTraversalGet(contour, &traversal);
    if (status != PRO_TK_NO_ERROR || traversal != PRO_CONTOUR_TRAV_EXTERNAL) return status;
    return ProContourEdgeVisit(placement->surface, contour, FindBoundaryEdges, NULL, data);
}

static ProError LocatePlateHole(PlateHolePlacement *placement, int extrude_id, double radius)
{
    ProFeature extrusion;
    ProUvStatus uv_status;
    ProError status;
    int axis;

    BridgeSetStage("find extrusion top face");
    status = ProFeatureInit(placement->solid, extrude_id, &extrusion);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProFeatureGeomitemVisit(&extrusion, PRO_SURFACE, FindTopFace, NULL, placement);
    if (status != PRO_TK_NO_ERROR) return status;
    if (placement->surface == NULL) return PRO_TK_E_NOT_FOUND;

    BridgeSetStage("check that the hole fits within the plate");
    status = ProSurfaceContourVisit(placement->surface, VisitOuterContour, NULL, placement);
    if (status != PRO_TK_NO_ERROR) return status;
    for (axis = 0; axis < 2; ++axis) {
        if (placement->lower_edges[axis] == NULL || placement->upper[axis] <= placement->lower[axis]) {
            return PRO_TK_INVALID_ITEM;
        }
        if (placement->center[axis] - radius <= placement->lower[axis] ||
            placement->center[axis] + radius >= placement->upper[axis]) {
            return PRO_TK_OUT_OF_RANGE;
        }
    }
    status = ProSurfaceParamEval(placement->solid, placement->surface, placement->center, placement->uv);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProSurfaceUvpntVerify(placement->solid, placement->surface, placement->uv, &uv_status);
    if (status != PRO_TK_NO_ERROR) return status;
    if (uv_status != PRO_UV_INSIDE) return PRO_TK_OUT_OF_RANGE;
    return PRO_TK_NO_ERROR;
}

static ProError AttachHoleElement(ProElement parent, ProElement child, ProError status)
{
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, child);
    if (status != PRO_TK_NO_ERROR) ProElementFree(&child);
    return status;
}

static ProError AddHoleCompound(ProElement parent, ProElemId id, ProElement *child)
{
    ProError status = ProElementAlloc(id, child);
    if (status != PRO_TK_NO_ERROR) return status;
    return AttachHoleElement(parent, *child, PRO_TK_NO_ERROR);
}

static ProError AddHoleInteger(ProElement parent, ProElemId id, int value)
{
    ProElement child;
    ProError status = ProElementAlloc(id, &child);
    if (status != PRO_TK_NO_ERROR) return status;
    return AttachHoleElement(parent, child, ProElementIntegerSet(child, value));
}

static ProError AddHoleDouble(ProElement parent, ProElemId id, double value)
{
    ProElement child;
    ProError status = ProElementAlloc(id, &child);
    if (status != PRO_TK_NO_ERROR) return status;
    return AttachHoleElement(parent, child, ProElementDoubleSet(child, value));
}

static ProError AddHoleReference(ProElement parent, ProElemId id, ProReference reference)
{
    ProElement child;
    ProError status = ProElementAlloc(id, &child);
    if (status != PRO_TK_NO_ERROR) return status;
    return AttachHoleElement(parent, child, ProElementReferenceSet(child, reference));
}

#define HOLE_TRY(call) do { status = (call); if (status != PRO_TK_NO_ERROR) goto cleanup; } while (0)

ProError BridgeHoleCreate(ProSolid solid, int extrude_id, double x_mm, double y_mm,
                          double diameter_mm, ProFeature *feature)
{
    PlateHolePlacement placement = {0};
    ProSelection face = NULL;
    ProSelection edge[2] = {NULL, NULL};
    ProReference face_reference = NULL;
    ProReference edge_reference[2] = {NULL, NULL};
    ProSelection model = NULL;
    ProModelitem model_item;
    ProGeomitem edge_item;
    ProElement tree = NULL;
    ProElement hole;
    ProElement depth;
    ProElement depth_to;
    ProElement depth_from;
    ProElement position;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {0};
    ProError status;
    int axis;
    static char detail[128];

    BridgeSetStage("validate hole inputs");
    if (solid == NULL || feature == NULL || extrude_id <= 0 ||
        !_finite(x_mm) || !_finite(y_mm) || !_finite(diameter_mm) || diameter_mm <= 0.0) {
        return PRO_TK_BAD_INPUTS;
    }

    placement.solid = solid;
    placement.center[0] = x_mm;
    placement.center[1] = y_mm;
    for (axis = 0; axis < 2; ++axis) {
        placement.lower[axis] = DBL_MAX;
        placement.upper[axis] = -DBL_MAX;
    }
    HOLE_TRY(LocatePlateHole(&placement, extrude_id, diameter_mm * 0.5));

    BridgeSetStage("select top face and offset edges");
    HOLE_TRY(ProSelectionAlloc(NULL, &placement.surface_item, &face));
    HOLE_TRY(ProSelectionUvParamSet(face, placement.uv));
    HOLE_TRY(ProSelectionPoint3dSet(placement.center, &face));
    HOLE_TRY(ProSelectionToReference(face, &face_reference));
    for (axis = 0; axis < 2; ++axis) {
        HOLE_TRY(ProEdgeToGeomitem(solid, placement.lower_edges[axis], &edge_item));
        HOLE_TRY(ProSelectionAlloc(NULL, &edge_item, &edge[axis]));
        HOLE_TRY(ProSelectionToReference(edge[axis], &edge_reference[axis]));
    }
    HOLE_TRY(ProMdlToModelitem((ProMdl)solid, &model_item));
    HOLE_TRY(ProSelectionAlloc(NULL, &model_item, &model));

    BridgeSetStage("build through-hole definition");
    HOLE_TRY(ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    HOLE_TRY(AddHoleInteger(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_HOLE));
    HOLE_TRY(AddHoleInteger(tree, PRO_E_FEATURE_FORM, PRO_HLE_TYPE_STRAIGHT));
    HOLE_TRY(AddHoleCompound(tree, PRO_E_HLE_COM, &hole));
    HOLE_TRY(AddHoleInteger(hole, PRO_E_HLE_TYPE_NEW, PRO_HLE_NEW_TYPE_STRAIGHT));
    HOLE_TRY(AddHoleInteger(hole, PRO_E_HLE_MAKE_LIGHTWT, PRO_HLE_REGULAR));
    HOLE_TRY(AddHoleDouble(hole, PRO_E_DIAMETER, diameter_mm));
    HOLE_TRY(AddHoleCompound(hole, PRO_E_HOLE_STD_DEPTH, &depth));
    HOLE_TRY(AddHoleCompound(depth, PRO_E_HOLE_DEPTH_TO, &depth_to));
    HOLE_TRY(AddHoleInteger(depth_to, PRO_E_HOLE_DEPTH_TO_TYPE, PRO_HLE_STRGHT_THRU_ALL_DEPTH));
    HOLE_TRY(AddHoleCompound(depth, PRO_E_HOLE_DEPTH_FROM, &depth_from));
    HOLE_TRY(AddHoleInteger(depth_from, PRO_E_HOLE_DEPTH_FROM_TYPE, PRO_HLE_STRGHT_NONE_DEPTH));

    BridgeSetStage("build hole placement definition");
    HOLE_TRY(AddHoleCompound(tree, PRO_E_HLE_PLACEMENT, &position));
    HOLE_TRY(AddHoleReference(position, PRO_E_HLE_PRIM_REF, face_reference));
    HOLE_TRY(AddHoleInteger(position, PRO_E_HLE_PL_TYPE, PRO_HLE_PL_TYPE_LIN));
    HOLE_TRY(AddHoleReference(position, PRO_E_HLE_DIM_REF1, edge_reference[0]));
    HOLE_TRY(AddHoleDouble(position, PRO_E_HLE_DIM_DIST1, x_mm - placement.lower[0]));
    HOLE_TRY(AddHoleReference(position, PRO_E_HLE_DIM_REF2, edge_reference[1]));
    HOLE_TRY(AddHoleDouble(position, PRO_E_HLE_DIM_DIST2, y_mm - placement.lower[1]));

    BridgeSetStage("create native through-hole");
    HOLE_TRY(ProArrayAlloc(1, sizeof(*options), 1, (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    status = ProFeatureWithoptionsCreate(model, tree, options, PRO_REGEN_NO_FLAGS, feature, &errors);
    if (status != PRO_TK_NO_ERROR && errors.error_number > 0 && errors.error_list != NULL) {
        sprintf_s(detail, sizeof(detail), "create native through-hole [element %d, error %d]",
                  errors.error_list[0].err_item_id, errors.error_list[0].error);
        BridgeSetStage(detail);
    }

cleanup:
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (tree != NULL) ProElementFree(&tree);
    if (model != NULL) ProSelectionFree(&model);
    if (face_reference != NULL) ProReferenceFree(face_reference);
    if (face != NULL) ProSelectionFree(&face);
    for (axis = 0; axis < 2; ++axis) {
        if (edge_reference[axis] != NULL) ProReferenceFree(edge_reference[axis]);
        if (edge[axis] != NULL) ProSelectionFree(&edge[axis]);
    }
    return status;
}
