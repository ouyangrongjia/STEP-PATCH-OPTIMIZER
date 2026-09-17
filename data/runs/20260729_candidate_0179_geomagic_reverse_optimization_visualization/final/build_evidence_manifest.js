#!/usr/bin/env node
"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const root = path.resolve(process.argv[2] || __dirname);
const manifestPath = path.join(root, "evidence_manifest.json");

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      return walk(absolute);
    }
    return [absolute];
  });
}

function role(relativePath) {
  const normalized = relativePath.replaceAll("\\", "/");
  if (normalized.startsWith("evidence/latest_layout_20260729_131649/")) {
    return "latest_layout_evidence";
  }
  if (normalized.startsWith("evidence/previous_full_diagnostic_20260729_032156/")) {
    return "historical_full_diagnostic_evidence";
  }
  if (normalized.startsWith("evidence/sewable_baseline_20260728_205420/")) {
    return "historical_sewable_baseline_evidence";
  }
  if (normalized.endsWith(".step")) {
    return "cad_step";
  }
  if (normalized.endsWith(".png")) {
    return "visualization";
  }
  if (normalized.endsWith(".json")) {
    return "machine_readable_evidence";
  }
  if (normalized.endsWith(".md")) {
    return "human_readable_evidence";
  }
  if (normalized.endsWith(".js")) {
    return "reproducibility_script";
  }
  return "other";
}

const files = walk(root)
  .filter((file) => path.resolve(file) !== path.resolve(manifestPath))
  .map((file) => {
    const bytes = fs.readFileSync(file);
    const relativePath = path.relative(root, file).replaceAll("\\", "/");
    return {
      path: relativePath,
      role: role(relativePath),
      bytes: bytes.length,
      sha256: crypto.createHash("sha256").update(bytes).digest("hex").toUpperCase(),
    };
  })
  .sort((left, right) => left.path.localeCompare(right.path, "en"));

const manifest = {
  schema_version: 1,
  run_id: "20260729_candidate_0179_geomagic_reverse_optimization_visualization",
  artifact_type: "CompositeEvidencePackage",
  manifest_self_excluded: true,
  product_validated: false,
  can_apply: false,
  latest_result: {
    status: "NoSewableGeometry",
    full_fit_count: 0,
    new_surface_step_available: false,
    preserved_exact_curve_network_json: "evidence/latest_layout_20260729_131649/curve_network.json",
    audit_snapshot: "evidence/latest_layout_20260729_131649/latest_layout_snapshot.json",
  },
  historical_step_evidence: {
    previous_full_diagnostic: {
      directory: "evidence/previous_full_diagnostic_20260729_032156",
      curve_network_step: "evidence/previous_full_diagnostic_20260729_032156/curve_network.step",
      untrimmed_support_surfaces_step:
        "evidence/previous_full_diagnostic_20260729_032156/untrimmed_support_surfaces.step",
      trimmed_patch_faces_step:
        "evidence/previous_full_diagnostic_20260729_032156/trimmed_patch_faces.step",
      diagnostic_sewn_step:
        "evidence/previous_full_diagnostic_20260729_032156/diagnostic_sewn.step",
      claim_boundary: "Historical DiagnosticOnly geometry; not the latest optimized result.",
    },
    sewable_baseline: {
      directory: "evidence/sewable_baseline_20260728_205420",
      claim_boundary: "Historical DiagnosticOnly geometry; not the latest optimized result.",
    },
  },
  files,
};

fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
process.stdout.write(`WROTE=${manifestPath}\nFILES=${files.length}\n`);
