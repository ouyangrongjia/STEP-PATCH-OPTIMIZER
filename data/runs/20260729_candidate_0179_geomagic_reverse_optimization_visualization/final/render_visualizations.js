#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const sharp = require("sharp");

function argument(name, required = true) {
  const index = process.argv.indexOf(`--${name}`);
  if (index >= 0 && index + 1 < process.argv.length) {
    return process.argv[index + 1];
  }
  if (required) {
    throw new Error(`缺少参数 --${name}`);
  }
  return null;
}

function readJson(filePath) {
  const text = fs.readFileSync(filePath, "utf8").replace(/^\uFEFF/, "");
  return JSON.parse(text);
}

function escapeXml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

const palette = {
  ink: "#172033",
  muted: "#667085",
  faint: "#98A2B3",
  paper: "#F7F8FA",
  white: "#FFFFFF",
  line: "#D8DEE8",
  blue: "#2477C9",
  blueDark: "#164E87",
  bluePale: "#EAF3FC",
  cyan: "#21A6B3",
  orange: "#E47D1B",
  orangeDark: "#A94F08",
  orangePale: "#FFF1E4",
  green: "#278357",
  greenPale: "#E7F5ED",
  amber: "#B76E00",
  amberPale: "#FFF4D7",
  grayPale: "#EEF1F5",
  red: "#B42318",
  redPale: "#FDECEA",
};

const fontFamily = "'Microsoft YaHei','Noto Sans CJK SC','SimHei',sans-serif";
const monoFamily = "'Cascadia Mono','Consolas',monospace";

function roundedRect(x, y, w, h, fill, stroke = "none", radius = 28, strokeWidth = 2) {
  return `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${radius}" fill="${fill}" stroke="${stroke}" stroke-width="${strokeWidth}"/>`;
}

function text(x, y, lines, options = {}) {
  const {
    size = 32,
    fill = palette.ink,
    weight = 400,
    anchor = "start",
    family = fontFamily,
    lineHeight = Math.round(size * 1.45),
    letterSpacing = 0,
    opacity = 1,
  } = options;
  const list = Array.isArray(lines) ? lines : [lines];
  const tspans = list
    .map((line, index) => `<tspan x="${x}" dy="${index === 0 ? 0 : lineHeight}">${escapeXml(line)}</tspan>`)
    .join("");
  return `<text x="${x}" y="${y}" text-anchor="${anchor}" font-family="${family}" font-size="${size}" font-weight="${weight}" letter-spacing="${letterSpacing}" fill="${fill}" opacity="${opacity}">${tspans}</text>`;
}

function badge(x, y, label, kind, width = 180) {
  const style = {
    pass: [palette.greenPale, palette.green],
    warn: [palette.amberPale, palette.amber],
    stop: [palette.redPale, palette.red],
    idle: [palette.grayPale, palette.muted],
    info: [palette.bluePale, palette.blueDark],
  }[kind];
  return [
    roundedRect(x, y, width, 54, style[0], "none", 27),
    text(x + width / 2, y + 37, label, { size: 26, weight: 700, fill: style[1], anchor: "middle" }),
  ].join("");
}

function svgDocument(width, height, body) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">
  <rect width="${width}" height="${height}" fill="${palette.paper}"/>
  ${body}
  </svg>`;
}

async function writePng(outputPath, width, height, body) {
  const svg = svgDocument(width, height, body);
  await sharp(Buffer.from(svg)).png({ compressionLevel: 9, adaptiveFiltering: true }).toFile(outputPath);
}

function statusRow(x, y, width, label, detail, kind, badgeText) {
  return [
    roundedRect(x, y, width, 82, palette.white, palette.line, 18, 2),
    text(x + 28, y + 34, label, { size: 27, weight: 700 }),
    text(x + 28, y + 68, detail, { size: 22, fill: palette.muted }),
    badge(x + width - 220, y + 14, badgeText, kind, 184),
  ].join("");
}

function getMetrics(latestDir, baselineDir, specPath, previousFlips, latestDegenerate) {
  const latestValidation = readJson(path.join(latestDir, "validation_summary.json"));
  const latestStatus = readJson(path.join(latestDir, "artifact_status.json"));
  const baselineValidation = readJson(path.join(baselineDir, "validation_summary.json"));
  const baselineStatus = readJson(path.join(baselineDir, "artifact_status.json"));
  const curve = readJson(path.join(latestDir, "curve_network.json"));
  const spec = readJson(specPath);
  const latestCandidate = latestValidation.layout.candidates[0];
  return {
    latestValidation,
    latestStatus,
    baselineValidation,
    baselineStatus,
    curve,
    spec,
    previousFlips,
    p995: latestCandidate.distortion_p995,
    maximumDistortion: latestCandidate.maximum_distortion,
    areaRatio: latestCandidate.minimum_to_median_uv_area_ratio,
    latestFlips: latestCandidate.flipped_triangles,
    latestDegenerate,
  };
}

function overview(metrics) {
  const w = 3200;
  const h = 1800;
  const old = metrics.baselineValidation;
  const now = metrics.latestValidation;
  let body = "";

  body += roundedRect(110, 80, 2980, 160, palette.blueDark, "none", 34);
  body += text(170, 150, "candidate-179｜Geomagic 逆向优化结果总览", {
    size: 58,
    weight: 700,
    fill: palette.white,
  });
  body += text(170, 205, "结论：参数布局改善，但新曲面尚未拟合", {
    size: 36,
    weight: 600,
    fill: "#D9EBFF",
  });

  const cardY = 300;
  const cardH = 620;
  const cardW = 1340;
  body += roundedRect(110, cardY, cardW, cardH, palette.white, palette.line, 30, 3);
  body += text(170, cardY + 72, "旧的可缝合基线｜仅诊断", { size: 42, weight: 700, fill: palette.blueDark });
  body += text(170, cardY + 118, "20260728_205420_144", { size: 25, fill: palette.muted, family: monoFamily });
  body += badge(1180, cardY + 42, "DiagnosticOnly", "info", 220);

  const oldRows = [
    ["曲线网络", "30 nodes / 33 curves / 5 internal", "pass", "已通过"],
    ["拟合几何", `${old.fitting.full_fit_count} 次完整拟合；${old.brep.retrim.replacement_faces} 张替换 Face`, "warn", "诊断产物"],
    ["缝合与 STEP", `0.001 mm；${old.step_roundtrip.roundtrip_solids} Solid / ${old.step_roundtrip.roundtrip_free_edges} 自由边`, "pass", "拓扑通过"],
    ["Creo", "导入 OK/PASS；ModelCHECK 未通过", "warn", "未验收"],
    ["产品 / Apply", `can_apply=${metrics.baselineStatus.can_apply}`, "stop", "不可应用"],
  ];
  oldRows.forEach((row, index) => {
    body += statusRow(170, cardY + 160 + index * 88, 1190, row[0], row[1], row[2], row[3]);
  });

  body += roundedRect(1750, cardY, cardW, cardH, palette.white, palette.line, 30, 3);
  body += text(1810, cardY + 72, "最新参数布局运行｜无新曲面", { size: 42, weight: 700, fill: palette.orangeDark });
  body += text(1810, cardY + 118, "20260729_131649_203", { size: 25, fill: palette.muted, family: monoFamily });
  body += badge(2810, cardY + 42, "Unavailable", "stop", 220);

  const nowRows = [
    ["曲线网络", `${metrics.curve.node_count} nodes / ${metrics.curve.edge_count} curves / ${metrics.curve.internal_edge_count} internal`, "pass", "已通过"],
    ["参数布局", `翻转 ${metrics.latestFlips}；退化 ${metrics.latestDegenerate}；p99.5 ${metrics.p995.toFixed(4)}`, "warn", "局部改善"],
    ["最大畸变", `${metrics.maximumDistortion.toFixed(3)} > 100`, "stop", "门禁失败"],
    ["正式拟合", `full_fit_count=${now.fitting.full_fit_count}；无支撑曲面 / Face`, "idle", "未执行"],
    ["结果状态", `${now.fitting.status}；can_apply=${metrics.latestStatus.can_apply}`, "stop", "不可应用"],
  ];
  nowRows.forEach((row, index) => {
    body += statusRow(1810, cardY + 160 + index * 88, 1190, row[0], row[1], row[2], row[3]);
  });

  body += roundedRect(110, 980, 2980, 420, palette.white, palette.line, 30, 3);
  body += text(170, 1048, "本次真正完成的改进", { size: 38, weight: 700 });
  body += text(170, 1162, `${metrics.previousFlips}  →  ${metrics.latestFlips}`, {
    size: 102,
    weight: 800,
    fill: palette.green,
    family: monoFamily,
  });
  body += text(170, 1220, "surfaceParameters 翻转 / 退化三角形", { size: 28, fill: palette.muted });
  body += text(1040, 1145, `p99.5 = ${metrics.p995.toFixed(4)}`, {
    size: 64,
    weight: 750,
    fill: palette.blueDark,
    family: monoFamily,
  });
  body += text(1040, 1220, `最小相对中位数 UV 面积 = ${metrics.areaRatio.toExponential(5)}`, {
    size: 28,
    fill: palette.muted,
  });
  body += text(2200, 1145, `max = ${metrics.maximumDistortion.toFixed(3)}`, {
    size: 64,
    weight: 750,
    fill: palette.red,
    family: monoFamily,
  });
  body += text(2200, 1220, "仍高于产品门 100，因此参数化整体未通过", {
    size: 28,
    fill: palette.red,
    weight: 600,
  });
  body += roundedRect(160, 1280, 2880, 76, palette.amberPale, "none", 16);
  body += text(200, 1330, "116 的起点来自此前完整诊断；左侧旧基线用于证明“曾有可缝合诊断几何”，两者不是同一指标快照。", {
    size: 27,
    fill: palette.amber,
    weight: 600,
  });

  body += roundedRect(110, 1450, 2980, 250, palette.redPale, "none", 30);
  body += text(170, 1525, "审阅边界", { size: 38, weight: 800, fill: palette.red });
  body += text(170, 1580, [
    "最新运行没有生成新支撑曲面、替换 Face、可缝合 B-Rep 或 STEP；旧基线几何不得冒充本次新结果。",
    "旧基线“已缝合且拓扑水密”不等于几何质量门通过，也不等于 Creo ModelCHECK 或产品 Apply 成功。",
    `逆向规格状态为 ${metrics.spec.status}，behavior_equivalent_ready=${metrics.spec.behavior_equivalent_ready}。`,
  ], { size: 28, fill: palette.ink, lineHeight: 42 });

  return [w, h, body];
}

function makeProjection(points, x, y, width, height, mode) {
  const center = [0, 1, 2].map((axis) => points.reduce((sum, p) => sum + p[axis], 0) / points.length);
  const raw = points.map((p) => {
    const px = p[0] - center[0];
    const py = p[1] - center[1];
    const pz = p[2] - center[2];
    if (mode === "side") {
      return [px, -pz];
    }
    return [0.72 * px - 0.69 * py, 0.38 * px + 0.40 * py - 0.83 * pz];
  });
  const xs = raw.map((p) => p[0]);
  const ys = raw.map((p) => p[1]);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const scale = Math.min(width / (maxX - minX || 1), height / (maxY - minY || 1));
  const usedW = (maxX - minX) * scale;
  const usedH = (maxY - minY) * scale;
  return (point) => {
    const px = point[0] - center[0];
    const py = point[1] - center[1];
    const pz = point[2] - center[2];
    const q = mode === "side"
      ? [px, -pz]
      : [0.72 * px - 0.69 * py, 0.38 * px + 0.40 * py - 0.83 * pz];
    return [
      x + (width - usedW) / 2 + (q[0] - minX) * scale,
      y + (height - usedH) / 2 + (q[1] - minY) * scale,
    ];
  };
}

function pathForEdge(edge, project) {
  return edge.samples
    .map((sample, index) => {
      const [x, y] = project(sample.point);
      return `${index === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`;
    })
    .join(" ");
}

function curveNetwork(metrics) {
  const w = 3000;
  const h = 2200;
  const curve = metrics.curve;
  const allPoints = curve.edges.flatMap((edge) => edge.samples.map((sample) => sample.point));
  const primary = makeProjection(allPoints, 160, 350, 1840, 1550, "iso");
  const side = makeProjection(allPoints, 2140, 670, 700, 620, "side");
  let body = "";

  body += text(140, 105, "最新曲线网络｜真实 curve_network.json 投影", { size: 56, weight: 750 });
  body += text(140, 165, "蓝色：28 条外边界曲线　橙色：5 条内部 RegularizedCubic　圆点：30 个网络节点", {
    size: 30,
    fill: palette.muted,
  });
  body += roundedRect(120, 230, 1940, 1770, palette.white, palette.line, 28, 3);
  body += text(170, 300, "三维等轴投影视图", { size: 34, weight: 700, fill: palette.blueDark });

  for (const edge of curve.edges.filter((item) => item.original_boundary)) {
    body += `<path d="${pathForEdge(edge, primary)}" fill="none" stroke="${palette.white}" stroke-width="16" stroke-linecap="round" stroke-linejoin="round"/>`;
    body += `<path d="${pathForEdge(edge, primary)}" fill="none" stroke="${palette.blue}" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>`;
  }
  for (const edge of curve.edges.filter((item) => !item.original_boundary)) {
    body += `<path d="${pathForEdge(edge, primary)}" fill="none" stroke="${palette.white}" stroke-width="24" stroke-linecap="round" stroke-linejoin="round"/>`;
    body += `<path d="${pathForEdge(edge, primary)}" fill="none" stroke="${palette.orange}" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>`;
  }
  for (const node of curve.nodes) {
    const [x, y] = primary(node.point);
    const fill = node.original_boundary ? palette.blueDark : palette.orangeDark;
    body += `<circle cx="${x.toFixed(2)}" cy="${y.toFixed(2)}" r="10" fill="${palette.white}" stroke="${fill}" stroke-width="5"/>`;
  }

  body += roundedRect(2120, 230, 760, 1340, palette.white, palette.line, 28, 3);
  body += text(2180, 305, "证据摘要", { size: 36, weight: 700 });
  body += text(2180, 385, [
    `节点：${curve.node_count}`,
    `曲线：${curve.edge_count}`,
    `外边界：${curve.boundary_edge_count}`,
    `内部曲线：${curve.internal_edge_count}`,
    `RegularizedCubic：${curve.regularized_cubic_edge_count}`,
  ], { size: 31, lineHeight: 54, family: monoFamily });
  body += badge(2180, 680, "投影门通过", "pass", 260);
  body += badge(2460, 680, "交叉门通过", "pass", 260);
  body += text(2180, 790, "最大曲线投影误差", { size: 28, fill: palette.muted });
  body += text(2180, 855, `${curve.maximum_projection_error.toFixed(6)} mm`, {
    size: 46,
    weight: 750,
    fill: palette.blueDark,
    family: monoFamily,
  });

  body += roundedRect(2180, 920, 640, 430, palette.grayPale, "none", 18);
  body += text(2220, 980, "X–Z 辅助视图", { size: 28, weight: 700 });
  for (const edge of curve.edges) {
    const color = edge.original_boundary ? palette.blue : palette.orange;
    const width = edge.original_boundary ? 5 : 9;
    body += `<path d="${pathForEdge(edge, side)}" fill="none" stroke="${color}" stroke-width="${width}" stroke-linecap="round" stroke-linejoin="round"/>`;
  }

  body += roundedRect(2120, 1610, 760, 390, palette.orangePale, "none", 28);
  body += text(2180, 1680, "解释限制", { size: 34, weight: 750, fill: palette.orangeDark });
  body += text(2180, 1740, [
    "这是曲线网络的二维投影，",
    "数据来自每条 edge.samples。",
    "它证明边界与内部曲线存在，",
    "不代表新支撑曲面已生成。",
  ], { size: 28, fill: palette.ink, lineHeight: 45 });

  body += text(140, 2100, "输入：最新 final/curve_network.json；外边界/内部分类直接读取 original_boundary 字段。", {
    size: 25,
    fill: palette.muted,
  });

  return [w, h, body];
}

function gatingMatrix(metrics) {
  const w = 3200;
  const h = 2100;
  const latest = metrics.latestValidation;
  const rows = [
    ["曲线网络", "closure / orientation / ownership / projection / intersection", "已通过", "pass", "30 nodes；33 curves；5 internal"],
    ["K=4 拓扑布局", "exact_target_reached / topology_passed", "已通过", "pass", "4 patches；无 pcurve 自交或非邻接交叉"],
    ["surfaceParameters 合法性", "翻转 / 退化", "已通过", "pass", `${metrics.previousFlips} → ${metrics.latestFlips}；退化 → ${metrics.latestDegenerate}`],
    ["布局畸变", "p99.5 / maximum / area ratio", "改善但未通过", "warn", `${metrics.p995.toFixed(4)} / ${metrics.maximumDistortion.toFixed(3)} / ${metrics.areaRatio.toExponential(5)}`],
    ["正式曲面拟合", "full fit / poles / knots / geometry error", "未执行", "idle", "full_fit_count=0；质量字段为 NotEvaluated"],
    ["支撑曲面与替换 Face", "support surfaces / retrim / lineage", "未执行", "idle", "没有新支撑曲面；replacement_faces=—"],
    ["B-Rep / 拓扑", "assembly / SameParameter / StrictTopology", "未执行", "idle", "没有可构造几何；不能评价水密性"],
    ["固定 0.001 mm 缝合", "diagnostic sewing", "未执行", "idle", "NoSewableGeometry；不是“0 条自由边”"],
    ["STEP 导出与回读", "export / roundtrip / BRepCheck", "未执行", "idle", "没有本次新 STEP"],
    ["Creo / ModelCHECK", "import / report / validation", "未执行", "idle", "creo_attempted=false"],
    ["产品 / Apply", "product_validated / can_apply", "未通过", "warn", "NoSewableGeometry；can_apply=false"],
  ];
  let body = "";

  body += roundedRect(100, 65, 3000, 170, palette.blueDark, "none", 32);
  body += text(160, 140, "最新运行｜布局与门控状态矩阵", { size: 56, weight: 750, fill: palette.white });
  body += text(160, 198, "三类状态严格区分：已通过、改善但未通过、未执行 / 不可评价", {
    size: 31,
    fill: "#D9EBFF",
  });

  body += roundedRect(100, 280, 3000, 100, palette.grayPale, "none", 18);
  body += text(145, 343, "阶段", { size: 30, weight: 700 });
  body += text(660, 343, "检查对象", { size: 30, weight: 700 });
  body += text(1790, 343, "状态", { size: 30, weight: 700 });
  body += text(2180, 343, "本次证据", { size: 30, weight: 700 });

  rows.forEach((row, index) => {
    const y = 395 + index * 132;
    const fill = index % 2 === 0 ? palette.white : "#FAFBFC";
    body += roundedRect(100, y, 3000, 116, fill, palette.line, 12, 2);
    body += text(145, y + 50, row[0], { size: 29, weight: 700 });
    body += text(660, y + 46, row[1], { size: 24, fill: palette.muted, family: monoFamily });
    body += badge(1740, y + 30, row[2], row[3], row[2] === "改善但未通过" ? 260 : 220);
    body += text(2180, y + 50, row[4], {
      size: 25,
      fill: row[3] === "warn" ? palette.amber : row[3] === "idle" ? palette.muted : palette.ink,
      weight: row[3] === "warn" ? 650 : 400,
    });
  });

  body += roundedRect(100, 1880, 3000, 130, palette.redPale, "none", 24);
  body += text(155, 1932, "重要：未执行阶段中的数值 0 是默认占位，不是“误差为 0”或“质量优良”的证据。", {
    size: 32,
    weight: 750,
    fill: palette.red,
  });
  body += text(155, 1980, `最终状态：${latest.fitting.status}；参数化整体未通过；没有新拟合几何。`, {
    size: 28,
    fill: palette.ink,
  });

  return [w, h, body];
}

async function main() {
  const latestDir = path.resolve(argument("latest"));
  const baselineDir = path.resolve(argument("baseline"));
  const specPath = path.resolve(argument("spec"));
  const outputDir = path.resolve(argument("output"));
  const previousFlips = Number(argument("previous-flips"));
  const latestDegenerate = Number(argument("latest-degenerate"));
  fs.mkdirSync(outputDir, { recursive: true });

  const metrics = getMetrics(latestDir, baselineDir, specPath, previousFlips, latestDegenerate);
  const outputs = [
    ["01_overview_comparison.png", overview(metrics)],
    ["02_latest_curve_network.png", curveNetwork(metrics)],
    ["03_layout_gating_matrix.png", gatingMatrix(metrics)],
  ];
  for (const [fileName, [width, height, body]] of outputs) {
    await writePng(path.join(outputDir, fileName), width, height, body);
    process.stdout.write(`${fileName}\t${width}x${height}\n`);
  }
}

main().catch((error) => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
