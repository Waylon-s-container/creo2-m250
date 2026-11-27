#ifndef CREO2_BRIDGE_H
#define CREO2_BRIDGE_H

#include <ProToolkit.h>
#include <ProSolid.h>
#include <ProFeature.h>

void BridgeSetStage(const char *stage);
ProError BridgeSketchCreate(ProSolid solid, double width_mm, double height_mm,
                            ProFeature *feature);
ProError BridgeExtrudeCreate(ProSolid solid, int sketch_id, double depth_mm,
                             ProFeature *feature);
ProError BridgeHoleCreate(ProSolid solid, int extrude_id, double x_mm,
                          double y_mm, double diameter_mm, ProFeature *feature);

#endif
