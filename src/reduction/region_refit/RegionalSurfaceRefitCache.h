#pragma once

#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/StepAdaptiveRegionSampler.h"

#include <filesystem>
#include <string>
#include <vector>

namespace spo {

class RegionalSurfaceRefitCache {
public:
    explicit RegionalSurfaceRefitCache(const RegionRefitRequest& request);

    bool enabled() const;
    const std::string& key() const;

    RegionRefitCacheEvent recordConstraintResult(bool success) const;
    bool loadSampling(
        StepAdaptiveRegionSamplingResult& result,
        RegionRefitCacheEvent& event) const;
    RegionRefitCacheEvent saveSampling(
        const StepAdaptiveRegionSamplingResult& result) const;
    bool loadCurveNetworks(
        const RegionTopologyConstraints& constraints,
        RegionCurveNetworkGenerationResult& result,
        RegionRefitCacheEvent& event) const;
    RegionRefitCacheEvent saveCurveNetworks(
        const RegionCurveNetworkGenerationResult& result) const;
    bool loadLayouts(
        RegionPatchLayoutGenerationResult& result,
        RegionRefitCacheEvent& event) const;
    RegionRefitCacheEvent saveLayouts(
        const RegionPatchLayoutGenerationResult& result) const;
    bool loadFit(
        const std::vector<RegionPatchLayout>& candidates,
        JointBSplineNetworkFitResult& result,
        RegionRefitCacheEvent& event) const;
    RegionRefitCacheEvent saveFit(
        const JointBSplineNetworkFitResult& result) const;

private:
    bool mayRead() const;
    bool mayWrite() const;
    std::filesystem::path cacheFile(const char* name) const;

    RegionRefitCachePolicy policy_ = RegionRefitCachePolicy::Disabled;
    std::filesystem::path entryDirectory_;
    std::string key_;
};

}
