#pragma once

#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "patch/ImportedPatchInfo.h"

#include <filesystem>

namespace spo {

class PatchImportService {
public:
    ImportedPatchInfo importPatch(const std::filesystem::path& patchPath) const;

    ImportedPatchInfo importPatchFromResult(
        const GeomagicAutoSurfaceResult& result) const;
};

}
