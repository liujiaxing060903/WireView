import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const outputPath = resolve(root, 'data/device-models/android-cn-mobile.json');
const reportPath = resolve(root, 'data/device-models/android-cn-mobile-report.md');
const runtimePath = resolve(root, 'data/device-models/android-cn-mobile.tsv');

const sources = [
  { file: 'brands/honor_cn.md', brand: 'HONOR', ecosystem: 'HONOR' },
  { file: 'brands/huawei_cn.md', brand: 'HUAWEI', ecosystem: 'HUAWEI' },
  { file: 'brands/oppo_cn.md', brand: 'OPPO', ecosystem: 'OPPO' },
  { file: 'brands/realme_cn.md', brand: 'realme', ecosystem: 'realme' },
  { file: 'brands/samsung_cn.md', brand: 'Samsung', ecosystem: 'Samsung' },
  { file: 'brands/vivo_cn.md', brand: 'vivo', ecosystem: 'vivo' },
  { file: 'brands/xiaomi_cn.md', brand: 'Xiaomi', ecosystem: 'Xiaomi' },
  { file: 'brands/oneplus.md', brand: 'OnePlus', ecosystem: 'OnePlus' },
  { file: 'brands/nubia.md', brand: 'nubia', ecosystem: 'nubia' },
  { file: 'brands/zte_cn.md', brand: 'ZTE', ecosystem: 'ZTE' },
];

const excludedDevicePattern = /手表|Watch|手环|Band|耳机|FreeBuds|FreeClip|音箱|Sound|智慧屏|智能屏|电视|TV|路由|Router|MateBook|笔记本|电脑|PC\b|显示器|打印机|眼镜|穿戴|车机|投影|手写笔|键盘|鼠标/i;
const tabletPattern = /平板|Pad|Tablet/i;
const normalizeModel = value => value
  .trim()
  .replace(/[‐‑‒–—−]/g, '-')
  .replace(/\s+/g, ' ')
  .toUpperCase();

function resolvedBrand(source, name) {
  if (source.ecosystem === 'Xiaomi' && /(^|\s)(REDMI|红米)/i.test(name)) return 'REDMI';
  if (source.ecosystem === 'vivo' && /(^|\s)iQOO\b/i.test(name)) return 'iQOO';
  return source.brand;
}

function makeRecord(source, lineNumber, section, model, name) {
  const context = `${section} ${name}`;
  return {
    model: model.trim(),
    normalizedModel: normalizeModel(model),
    name: name.trim(),
    brand: resolvedBrand(source, name),
    ecosystem: source.ecosystem,
    category: tabletPattern.test(context) ? 'tablet' : 'phone',
    source: source.file,
    sourceLine: lineNumber,
    sourceSection: section,
  };
}

async function parseSource(source) {
  const text = await readFile(resolve(root, source.file), 'utf8');
  const records = [];
  const excluded = [];
  let section = '未分组';

  for (const [index, line] of text.split(/\r?\n/).entries()) {
    const sectionMatch = line.match(/^##\s+(.+)$/);
    if (sectionMatch) {
      section = sectionMatch[1].trim();
      continue;
    }

    const entryMatch = line.match(/^`([^`]+)`:\s*(.+?)\s*$/);
    if (!entryMatch) continue;
    const [, model, name] = entryMatch;
    if (!/[\p{L}\p{N}]/u.test(model)) continue;

    const record = makeRecord(source, index + 1, section, model, name);
    if (excludedDevicePattern.test(`${section} ${name}`)) {
      excluded.push(record);
    } else {
      records.push(record);
    }
  }

  return { records, excluded };
}

const parsed = await Promise.all(sources.map(parseSource));
const allRecords = parsed.flatMap(item => item.records);
const excluded = parsed.flatMap(item => item.excluded);
const grouped = new Map();
for (const record of allRecords) {
  const bucket = grouped.get(record.normalizedModel) ?? [];
  bucket.push(record);
  grouped.set(record.normalizedModel, bucket);
}

const collisions = [...grouped.values()]
  .filter(records => new Set(records.map(record => `${record.name}|${record.brand}|${record.category}`)).size > 1)
  .sort((left, right) => left[0].normalizedModel.localeCompare(right[0].normalizedModel));
const models = [...grouped.values()]
  .map(records => records[0])
  .sort((left, right) => left.normalizedModel.localeCompare(right.normalizedModel, 'en'));
const countsByBrand = Object.groupBy(models, record => record.brand);
const countsByCategory = Object.groupBy(models, record => record.category);

const catalog = {
  schemaVersion: 1,
  generatedAt: new Date().toISOString(),
  scope: {
    platforms: ['android'],
    regions: ['CN'],
    ecosystems: ['HONOR', 'HUAWEI', 'OPPO', 'realme', 'Samsung', 'vivo', 'Xiaomi', 'OnePlus', 'nubia', 'ZTE'],
    includedCategories: ['phone', 'tablet'],
    excludedCategories: ['wearable', 'audio', 'computer', 'display', 'tv', 'router', 'other'],
  },
  lookupPolicy: {
    connectedDeviceCache: '优先按已连接设备的稳定 Android 标识读取本地映射；未命中时再按 normalizedModel 查询本目录。',
    normalization: '去除首尾空白、统一连字符、压缩内部空白并转换为大写。',
  },
  sources: sources.map(source => source.file),
  models,
};

const report = [
  '# Android 国行手机与平板型号映射：生成报告',
  '',
  '此文件由 `tools/generate_device_models.mjs` 从 `brands/` 原始资料生成。不要手工编辑 JSON；修改筛选规则后重新生成即可。',
  '',
  '## 范围',
  '',
  '- 品牌生态：荣耀、华为、OPPO、realme、三星、vivo/iQOO、小米/REDMI、OnePlus、努比亚/红魔、中兴。',
  '- 保留：手机、平板。',
  '- 排除：穿戴、音频、电脑、显示器、电视、路由器等非 Android 副屏目标设备。',
  '- 查找顺序：先查已连接设备的本地缓存；缓存未命中时再按 `normalizedModel` 查询目录。',
  '',
  '## 统计',
  '',
  `- 去重后型号：${models.length}`, 
  `- 原始条目：${allRecords.length}`,
  `- 被规则排除：${excluded.length}`,
  `- 需要人工确认的同型号冲突：${collisions.length}`,
  '',
  '### 按品牌',
  '',
  ...Object.entries(countsByBrand).sort(([a], [b]) => a.localeCompare(b, 'en')).map(([brand, records]) => `- ${brand}: ${records.length}`),
  '',
  '### 按类型',
  '',
  ...Object.entries(countsByCategory).sort(([a], [b]) => a.localeCompare(b, 'en')).map(([category, records]) => `- ${category}: ${records.length}`),
  '',
  '## 同型号冲突（保留首条，需后续按 ADB 标识细化）',
  '',
  ...(collisions.length === 0 ? ['无。'] : collisions.map(records => `- \`${records[0].normalizedModel}\`：${records.map(record => `${record.brand} ${record.name}（${record.source}:${record.sourceLine}）`).join('；')}`)),
  '',
  '## 已排除条目示例',
  '',
  ...(excluded.length === 0 ? ['无。'] : excluded.slice(0, 40).map(record => `- ${record.brand} ${record.name}（\`${record.model}\`，${record.source}:${record.sourceLine}）`)),
  '',
].join('\n');

await mkdir(dirname(outputPath), { recursive: true });
await writeFile(outputPath, `${JSON.stringify(catalog, null, 2)}\n`, 'utf8');
await writeFile(runtimePath, [
  '# normalizedModel\tname\tcategory',
  ...models.map(record => [record.normalizedModel, record.name, record.category]
    .map(value => value.replace(/[\t\r\n]+/g, ' ')).join('\t')),
  '',
].join('\n'), 'utf8');
await writeFile(reportPath, report, 'utf8');
console.log(`Generated ${models.length} unique models; ${excluded.length} excluded; ${collisions.length} collisions.`);
