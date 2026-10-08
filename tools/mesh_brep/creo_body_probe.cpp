#include <ProCore.h>
#include <ProMdl.h>
#include <ProArray.h>
#include <ProSolidBody.h>
#include <ProQuilt.h>
#include <ProSurface.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <algorithm>
#include <set>

extern "C" ProError user_initialize(int, char**, char*, char*, wchar_t[80]) { return PRO_TK_NO_ERROR; }
extern "C" void user_terminate() {}

ProError count_surface(ProSurface, ProError status, ProAppData data) {
    if (status == PRO_TK_NO_ERROR) ++*static_cast<int*>(data);
    return PRO_TK_NO_ERROR;
}
ProError surface_ids(ProSurface surface, ProError status, ProAppData data) {
    int id = 0;
    if (status == PRO_TK_NO_ERROR && ProSurfaceIdGet(surface, &id) == PRO_TK_NO_ERROR)
        static_cast<std::set<int>*>(data)->insert(id);
    return PRO_TK_NO_ERROR;
}
struct QuiltStats { std::ostream* out; int count = 0; int external = 0; int closed = 0; std::set<int> surfaces; };
ProError count_quilt(ProQuilt quilt, ProError status, ProAppData data) {
    if (status != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    auto& info = *static_cast<QuiltStats*>(data);
    if (info.count++) *info.out << ',';
    ProBoolean internal = PRO_B_FALSE, backup = PRO_B_FALSE;
    ProError internal_status = ProQuiltIsInternal(quilt, &internal);
    ProError backup_status = ProQuiltIsBackupgeometry(quilt, &backup);
    double volume = 0.; ProError volume_status = ProQuiltVolumeEval(quilt, &volume);
    int surfaces = 0; ProQuiltSurfaceVisit(quilt, count_surface, nullptr, &surfaces);
    ProQuiltSurfaceVisit(quilt, surface_ids, nullptr, &info.surfaces);
    if (volume_status == PRO_TK_NO_ERROR) ++info.closed;
    if (!internal && !backup) ++info.external;
    *info.out << "{\"internal_status\":" << internal_status << ",\"internal\":" << int(internal)
        << ",\"backup_status\":" << backup_status << ",\"backup\":" << int(backup)
        << ",\"volume_status\":" << volume_status << ",\"volume_model_units\":" << volume
        << ",\"surfaces\":" << surfaces << '}';
    return PRO_TK_NO_ERROR;
}

// Reads an already imported native part. Never modifies geometry or imports a
// reference model. The caller supplies a task-owned Creo launch command.
int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "creo_body_probe <creo-command> <part> <result.json>\n"; return 2; }
    std::ofstream out(std::filesystem::u8path(argv[3]));
    if (!out) return 2;
    ProError start = ProEngineerStart(argv[1], nullptr);
    out << "{\"start_status\":" << start;
    out.flush();
    if (start != PRO_TK_NO_ERROR) { out << "}\n"; return 3; }
    const auto path = std::filesystem::absolute(std::filesystem::u8path(argv[2])).wstring();
    ProPath full{};
    if (path.size() >= sizeof(full)/sizeof(wchar_t)) { ProEngineerEnd(); out << ",\"path_too_long\":true}\n"; return 2; }
    std::copy(path.begin(), path.end(), full);
    ProMdl model = nullptr;
    ProError load = ProMdlFiletypeLoad(full, PRO_MDLFILE_UNUSED, PRO_B_FALSE, &model);
    out << ",\"load_status\":" << load;
    if (load != PRO_TK_NO_ERROR) { ProEngineerEnd(); out << "}\n"; return 4; }
    ProSolid solid = reinterpret_cast<ProSolid>(model);
    out << ",\"quilts\":["; QuiltStats quilts{&out};
    ProError quilt_status = ProSolidQuiltVisit(solid, count_quilt, nullptr, &quilts);
    ProMassProperty total_mass{};
    ProError total_status = ProSolidMassPropertyWithDensityGet(solid, nullptr, PRO_MP_DENS_USE_ALWAYS, 1., &total_mass);
    out << "],\"unit_density_mass_status\":" << total_status << ",\"volume_model_units\":" << total_mass.volume;
    ProSolidBody* bodies = nullptr;
    ProError collect = ProSolidBodiesCollect(solid, &bodies);
    int n = 0;
    if (collect == PRO_TK_NO_ERROR && bodies) ProArraySizeGet(reinterpret_cast<ProArray>(bodies), &n);
    out << ",\"quilt_visit_status\":" << quilt_status << ",\"external_quilts\":" << quilts.external
        << ",\"body_collect_status\":" << collect << ",\"bodies\":[";
    int active = 0; std::set<int> body_surfaces;
    for (int i = 0; i < n; ++i) {
        if (i) out << ',';
        ProSolidBodyState state = PRO_BODY_STATE_NONE;
        ProError state_status = ProSolidBodyStateGet(&bodies[i], &state);
        int surfaces = 0;
        ProError surface_status = ProSolidBodySurfaceVisit(&bodies[i], count_surface, &surfaces);
        if (state == PRO_BODY_STATE_ACTIVE)
            ProSolidBodySurfaceVisit(&bodies[i], surface_ids, &body_surfaces);
        ProMassProperty mass{};
        ProError mass_status = ProSolidBodyMassPropertyGet(&bodies[i], nullptr, &mass);
        if (state_status == PRO_TK_NO_ERROR && state == PRO_BODY_STATE_ACTIVE && surfaces > 0) ++active;
        out << "{\"id\":" << bodies[i].id << ",\"state_status\":" << state_status
            << ",\"state\":" << state << ",\"surface_visit_status\":" << surface_status
            << ",\"surfaces\":" << surfaces << ",\"mass_status\":" << mass_status
            << ",\"volume_model_units\":" << mass.volume << '}';
    }
    if (bodies) ProArrayFree(reinterpret_cast<ProArray*>(&bodies));
    int orphan_surfaces = 0;
    for (int id : quilts.surfaces) if (!body_surfaces.count(id)) ++orphan_surfaces;
    out << "],\"active_bodies_with_surfaces\":" << active
        << ",\"quilt_surfaces_outside_active_bodies\":" << orphan_surfaces
        << ",\"closed_quilts\":" << quilts.closed
        << ",\"native_closed_solid\":"
        << ((active == 1 && orphan_surfaces == 0 && quilts.closed == quilts.count &&
             total_status == PRO_TK_NO_ERROR && total_mass.volume > 0) ? "true" : "false") << "}\n";
    out.flush(); ProEngineerEnd(); return 0;
}
