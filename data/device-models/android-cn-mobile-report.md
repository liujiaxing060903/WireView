# Android 国行手机与平板型号映射：生成报告

此文件由 `tools/generate_device_models.mjs` 从 `brands/` 原始资料生成。不要手工编辑 JSON；修改筛选规则后重新生成即可。

## 范围

- 品牌生态：荣耀、华为、OPPO、realme、三星、vivo/iQOO、小米/REDMI、OnePlus、努比亚/红魔、中兴。
- 保留：手机、平板。
- 排除：穿戴、音频、电脑、显示器、电视、路由器等非 Android 副屏目标设备。
- 查找顺序：先查已连接设备的本地缓存；缓存未命中时再按 `normalizedModel` 查询目录。

## 统计

- 去重后型号：2591
- 原始条目：2703
- 被规则排除：732
- 需要人工确认的同型号冲突：101

### 按品牌

- HONOR: 489
- HUAWEI: 458
- iQOO: 86
- nubia: 113
- OnePlus: 193
- OPPO: 202
- realme: 104
- REDMI: 200
- Samsung: 306
- vivo: 216
- Xiaomi: 157
- ZTE: 67

### 按类型

- phone: 2323
- tablet: 268

## 同型号冲突（保留首条，需后续按 ADB 标识细化）

- `2109119BC`：Xiaomi Xiaomi Civi（brands/xiaomi_cn.md:447）；Xiaomi Xiaomi Civi 1S（brands/xiaomi_cn.md:451）
- `23073RPBFC`：REDMI Redmi Pad SE（brands/xiaomi_cn.md:1257）；REDMI Redmi Pad M7（brands/xiaomi_cn.md:1261）
- `23076RA4BC`：REDMI Redmi 12R（brands/xiaomi_cn.md:733）；REDMI Redmi Note 12R（brands/xiaomi_cn.md:937）
- `AGM3-AL09HN`：HONOR 荣耀平板 7 10.1 英寸 LTE 版（brands/honor_cn.md:1137）；HONOR 荣耀平板 Z5 10.1 英寸 LTE 版（brands/honor_cn.md:1319）
- `AGM3-W09HN`：HONOR 荣耀平板 7 10.1 英寸 Wi-Fi 版（brands/honor_cn.md:1135）；HONOR 荣耀平板 X8 10.1 英寸 Wi-Fi 版（brands/honor_cn.md:1269）；HONOR 荣耀平板 Z5 10.1 英寸 Wi-Fi 版（brands/honor_cn.md:1317）
- `ALN-AL10`：HUAWEI HUAWEI Mate 60 Pro+（brands/huawei_cn.md:200）；HUAWEI HUAWEI Mate 60 RS 非凡大师（brands/huawei_cn.md:204）
- `ALT-AN00`：HONOR 荣耀 Play9T 5G（brands/honor_cn.md:829）；HONOR 荣耀畅玩 60 Plus 5G（brands/honor_cn.md:1079）
- `BLA-AL00`：HUAWEI HUAWEI Mate 10 Pro 全网通版（brands/huawei_cn.md:80）；HUAWEI HUAWEI Mate 10 保时捷设计（brands/huawei_cn.md:86）
- `BRT-W09`：HONOR 荣耀平板 V7 Pro 11 英寸 Wi-Fi 版（brands/honor_cn.md:1195）；HONOR 荣耀平板 V8 11 英寸 Wi-Fi 版（brands/honor_cn.md:1201）
- `BVL-AN20`：HONOR 荣耀 Magic6 至臻版（brands/honor_cn.md:95）；HONOR 荣耀 Magic6 RSR 保时捷设计（brands/honor_cn.md:99）
- `CLK-AN00`：HONOR 荣耀 Play8T 5G（brands/honor_cn.md:821）；HONOR 荣耀畅玩 50 Plus 5G（brands/honor_cn.md:1067）
- `DBY-W09`：HUAWEI HUAWEI MatePad 11 英寸 Wi-Fi 版（brands/huawei_cn.md:1406）；HUAWEI 华为平板 C7 10.95 英寸 Wi-Fi 版（brands/huawei_cn.md:1616）
- `DCO-AL00`：HUAWEI HUAWEI Mate 50 Pro（brands/huawei_cn.md:184）；HUAWEI HUAWEI Mate 50 RS 保时捷设计（brands/huawei_cn.md:188）
- `DIO-AN00`：HONOR 荣耀 X40i（brands/honor_cn.md:669）；HONOR 荣耀 Play7T Pro 5G（brands/honor_cn.md:817）
- `ELE2-W19`：HONOR 荣耀平板 X10 Pro 柔光版 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1293）；HONOR 荣耀护眼学习机 火火兔版 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1305）
- `ELN-W09`：HONOR 荣耀平板 X8 Pro 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1273）；HONOR 荣耀平板 Z6 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1327）
- `ELN2-W09`：HONOR 荣耀平板 X9 Pro 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1283）；HONOR 荣耀平板 Z6 第二代 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1331）
- `GLO-G561`：HONOR 荣耀 MagicBook 14 2023 i5 (16GB+1TB)（brands/honor_cn.md:1509）；HONOR 荣耀 MagicBook 14 Pro 2023 i5 (16GB+1TB)（brands/honor_cn.md:1515）
- `HYM-W76`：HONOR 荣耀 MagicBook 16 2021 R7（brands/honor_cn.md:1573）；HONOR 荣耀 MagicBook 16 Pro 2021 R7（brands/honor_cn.md:1577）
- `JDY-AN00`：HONOR 荣耀 Play9C 5G（brands/honor_cn.md:837）；HONOR 荣耀畅玩 50 5G / 荣耀畅玩 50m 5G（brands/honor_cn.md:1063）
- `JLN-AL00`：HUAWEI HUAWEI nova 9 SE（brands/huawei_cn.md:706）；HUAWEI HUAWEI nova 10 青春版（brands/huawei_cn.md:726）
- `LAB-AN00`：HONOR 荣耀 Play11T 5G（brands/honor_cn.md:853）；HONOR 荣耀畅玩 80 Plus 5G（brands/honor_cn.md:1091）
- `LLY-AN00`：HONOR 荣耀 X50i+（brands/honor_cn.md:689）；HONOR 荣耀 Play8T Pro 5G（brands/honor_cn.md:825）
- `LMR-AL10`：HUAWEI HUAWEI Pura 80 Pro+（brands/huawei_cn.md:494）；HUAWEI HUAWEI Pura 80 Ultra（brands/huawei_cn.md:498）
- `LNA-AN00`：HONOR 荣耀 X80i（brands/honor_cn.md:729）；HONOR 荣耀 Play11 Plus 5G / 荣耀 Play11 Pro 5G（brands/honor_cn.md:857）
- `LOG-AN10`：HONOR 荣耀 Play10 5G / 荣耀 Play10T 5G（brands/honor_cn.md:841）；HONOR 荣耀畅玩 80 Pro 5G（brands/honor_cn.md:1095）
- `LON-AL00`：HUAWEI HUAWEI Mate 9 Pro（brands/huawei_cn.md:66）；HUAWEI HUAWEI Mate 9 保时捷设计（brands/huawei_cn.md:70）
- `LYN-AN00`：HONOR 荣耀 X60i（brands/honor_cn.md:703）；HONOR 荣耀 Play9T Pro 5G（brands/honor_cn.md:833）
- `M1903F11A`：REDMI Redmi K20 Pro 全网通版（brands/xiaomi_cn.md:1073）；REDMI Redmi K20 Pro 尊享版（brands/xiaomi_cn.md:1079）
- `M2001G7AC`：REDMI Redmi K30 5G（brands/xiaomi_cn.md:1085）；REDMI Redmi K30i 5G（brands/xiaomi_cn.md:1089）
- `M2012K11C`：REDMI Redmi K40 Pro（brands/xiaomi_cn.md:1119）；REDMI Redmi K40 Pro+（brands/xiaomi_cn.md:1123）
- `MGA-AL40`：HUAWEI 华为畅享 60（brands/huawei_cn.md:1104）；HUAWEI 华为畅享 70z（brands/huawei_cn.md:1124）
- `MNA-AL00`：HUAWEI HUAWEI P60 Pro（brands/huawei_cn.md:460）；HUAWEI HUAWEI P60 Art（brands/huawei_cn.md:464）
- `MTN-AN00`：HONOR 荣耀 X70（brands/honor_cn.md:711）；HONOR 荣耀 X70 Pro Max（brands/honor_cn.md:717）
- `NBL-WAQ9HNRP`：HONOR 荣耀 MagicBook 14 R5 (16GB+512GB)（brands/honor_cn.md:1451）；HONOR 荣耀 MagicBook 14 SE 2021 R5 (16GB+512GB)（brands/honor_cn.md:1481）
- `NIC-AN00`：HONOR 荣耀 Play10C 5G（brands/honor_cn.md:845）；HONOR 荣耀畅玩 60 5G / 荣耀畅玩 60m 5G（brands/honor_cn.md:1071）
- `NLA-AN00`：HONOR 荣耀 Play10A 5G / 荣耀 Play11C 5G（brands/honor_cn.md:849）；HONOR 荣耀畅玩 60A 5G / 荣耀畅玩 80 5G / 荣耀畅玩 80m 5G（brands/honor_cn.md:1075）
- `NOP-AN00`：HUAWEI HUAWEI Mate 40 Pro+ 5G（brands/huawei_cn.md:168）；HUAWEI HUAWEI Mate 40 RS 保时捷设计（brands/huawei_cn.md:172）
- `NP02J`：nubia nubia Pad 3D II（brands/nubia.md:351）；nubia 红魔平板 · 3D 探索版 5G（brands/nubia.md:471）
- `NX629J`：nubia 红魔 3（brands/nubia.md:373）；nubia 红魔 3S（brands/nubia.md:377）
- `NX659J`：nubia 红魔 5G（brands/nubia.md:381）；nubia 红魔 5S（brands/nubia.md:385）
- `NX769J`：nubia 红魔 9 Pro / 红魔 9 Pro+（brands/nubia.md:429）；nubia 红魔 9S Pro / 红魔 9S Pro+（brands/nubia.md:433）
- `NX789J`：nubia 红魔 10 Pro / 红魔 10 Pro+（brands/nubia.md:437）；nubia 红魔 10S Pro / 红魔 10S Pro+（brands/nubia.md:445）
- `NX809J`：nubia 红魔 11 Pro / 红魔 11 Pro+（brands/nubia.md:449）；nubia 红魔 11S Pro / 红魔 11S Pro+（brands/nubia.md:457）
- `PCLM50`：OPPO OPPO Reno3 元气版 全网通版（brands/oppo_cn.md:195）；OPPO OPPO K7（brands/oppo_cn.md:695）
- `PDVM00`：OPPO OPPO A11s（brands/oppo_cn.md:463）；OPPO OPPO A32（brands/oppo_cn.md:467）
- `PEEM00`：OPPO OPPO Find X3 Pro（brands/oppo_cn.md:35）；OPPO OPPO Find X3 Pro 摄影师版（brands/oppo_cn.md:39）
- `PFTM20`：OPPO OPPO A56s（brands/oppo_cn.md:505）；OPPO OPPO A57 (2022)（brands/oppo_cn.md:509）
- `PHJ110`：OPPO OPPO A58（brands/oppo_cn.md:513）；OPPO OPPO A58x（brands/oppo_cn.md:517）；OPPO OPPO A1x / OPPO A1 活力版（brands/oppo_cn.md:577）
- `PKV110`：OPPO OPPO A5 活力版（brands/oppo_cn.md:625）；OPPO OPPO K13x（brands/oppo_cn.md:759）
- `PLL110`：OPPO OPPO A6 GT / OPPO A6 Max / OPPO A6l（brands/oppo_cn.md:641）；OPPO OPPO K13s（brands/oppo_cn.md:755）
- `RMX2200`：realme 真我 Q2i 5G（brands/realme_cn.md:207）；realme 真我 V3 5G 全网通版（brands/realme_cn.md:339）
- `ROD-W09`：HONOR 荣耀平板 9 Pro 12.1 英寸 Wi-Fi 版（brands/honor_cn.md:1151）；HONOR 荣耀平板 V8 Pro 12.1 英寸 Wi-Fi 版（brands/honor_cn.md:1205）
- `ROL-W00`：HONOR 荣耀平板 10 Pro 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1163）；HONOR 荣耀平板 V9 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1209）
- `ROL-W10`：HONOR 荣耀平板 10 Pro 柔光版 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1165）；HONOR 荣耀平板 V9 柔光版 11.5 英寸 Wi-Fi 版（brands/honor_cn.md:1211）
- `SER-AN00`：HONOR 荣耀 WIN Turbo（brands/honor_cn.md:545）；HONOR 荣耀 Power2（brands/honor_cn.md:869）
- `SPN-AL00`：HUAWEI HUAWEI nova 5i Pro 全网通版（brands/huawei_cn.md:624）；HUAWEI HUAWEI nova 5z 全网通版（brands/huawei_cn.md:630）
- `SPN-TL00`：HUAWEI HUAWEI nova 5i Pro 移动 4G+ 版（brands/huawei_cn.md:626）；HUAWEI HUAWEI nova 5z 移动 4G+ 版（brands/huawei_cn.md:632）
- `V1730EA`：vivo vivo Y89（brands/vivo_cn.md:751）；vivo vivo Z1 青春版（brands/vivo_cn.md:891）
- `V1731CA`：vivo vivo Y71s（brands/vivo_cn.md:675）；vivo vivo Y73（brands/vivo_cn.md:687）
- `V1732A`：vivo vivo Y81 全网通版（brands/vivo_cn.md:739）；vivo vivo Y81s 全网通版（brands/vivo_cn.md:745）
- `V1732T`：vivo vivo Y81 移动全网通版（brands/vivo_cn.md:741）；vivo vivo Y81s 移动全网通版（brands/vivo_cn.md:747）
- `V1813A`：vivo vivo Y97 全网通版（brands/vivo_cn.md:779）；vivo vivo Z3i 全网通版（brands/vivo_cn.md:901）；vivo vivo Z3i 标准版 全网通版（brands/vivo_cn.md:907）
- `V1813T`：vivo vivo Y97 移动全网通版（brands/vivo_cn.md:781）；vivo vivo Z3i 移动全网通版（brands/vivo_cn.md:903）；vivo vivo Z3i 标准版 移动全网通版（brands/vivo_cn.md:909）
- `V1818A`：vivo vivo Y93 全网通版（brands/vivo_cn.md:761）；vivo vivo U1（brands/vivo_cn.md:945）
- `V1818CA`：vivo vivo Y91 全网通版（brands/vivo_cn.md:755）；vivo vivo Y93s 全网通版（brands/vivo_cn.md:773）
- `V1818CT`：vivo vivo Y91 移动全网通版（brands/vivo_cn.md:757）；vivo vivo Y93 标准版 移动全网通版（brands/vivo_cn.md:769）；vivo vivo Y93s 移动全网通版（brands/vivo_cn.md:775）
- `V1901A`：vivo vivo Y3 全网通版（brands/vivo_cn.md:435）；vivo vivo Y3s 全网通版（brands/vivo_cn.md:447）
- `V1901T`：vivo vivo Y3 移动全网通版（brands/vivo_cn.md:437）；vivo vivo Y3s 移动全网通版（brands/vivo_cn.md:449）
- `V1941A`：vivo vivo Z5i 全网通版（brands/vivo_cn.md:933）；vivo vivo U3 全网通版（brands/vivo_cn.md:949）
- `V1941T`：vivo vivo Z5i 移动全网通版（brands/vivo_cn.md:935）；vivo vivo U3 移动全网通版（brands/vivo_cn.md:951）
- `V2002A`：vivo vivo Y51s 5G（brands/vivo_cn.md:599）；vivo vivo Y70s 5G（brands/vivo_cn.md:667）；vivo vivo Y70t 5G（brands/vivo_cn.md:671）
- `V2031A`：vivo vivo S7e（brands/vivo_cn.md:305）；vivo vivo Y73s 5G（brands/vivo_cn.md:691）
- `V2054A`：vivo vivo Y31s 5G（brands/vivo_cn.md:511）；vivo vivo Y31s (t2 版) 5G（brands/vivo_cn.md:523）；vivo vivo Y52s (t1 版) 5G（brands/vivo_cn.md:607）
- `V2056A`：vivo vivo X60 Pro+（brands/vivo_cn.md:115）；vivo vivo X60t Pro+（brands/vivo_cn.md:119）
- `V2068A`：vivo vivo Y31s 标准版 5G（brands/vivo_cn.md:515）；vivo vivo Y31s (t1 版) 5G（brands/vivo_cn.md:519）
- `V2069A`：vivo vivo Y53s (t1 版) 5G（brands/vivo_cn.md:619）；vivo vivo Y53s (NFC 版) 5G（brands/vivo_cn.md:627）
- `V2121A`：vivo vivo S10（brands/vivo_cn.md:321）；vivo vivo S10 Pro（brands/vivo_cn.md:325）
- `V2123A`：vivo vivo Y53s (t2 版) 5G（brands/vivo_cn.md:623）；vivo vivo T1x（brands/vivo_cn.md:867）
- `V2164A`：vivo vivo Y55s 5G（brands/vivo_cn.md:639）；vivo vivo Y72t 5G（brands/vivo_cn.md:683）
- `V2166A`：vivo vivo Y33s 5G（brands/vivo_cn.md:539）；vivo vivo Y33e 5G（brands/vivo_cn.md:543）；vivo vivo Y52t 5G（brands/vivo_cn.md:611）
- `V2168A`：vivo vivo Y10 (t1 版)（brands/vivo_cn.md:475）；vivo vivo Y32t (P70)（brands/vivo_cn.md:531）
- `V2180A`：vivo vivo Y10 (t2 版)（brands/vivo_cn.md:479）；vivo vivo Y32t (骁龙 680)（brands/vivo_cn.md:535）
- `V2230A`：vivo vivo Y35 5G / vivo Y35m 5G（brands/vivo_cn.md:551）；vivo vivo Y53t 5G（brands/vivo_cn.md:631）
- `V2279A`：vivo vivo Y35+ 5G / vivo Y35m+ 5G（brands/vivo_cn.md:555）；vivo vivo Y55t 5G（brands/vivo_cn.md:643）；vivo vivo Y78 (t1) 5G / vivo Y78m (t1) 5G（brands/vivo_cn.md:731）；vivo vivo Y100i 5G（brands/vivo_cn.md:793）
- `V2312BA`：vivo vivo Y78t 5G（brands/vivo_cn.md:735）；vivo vivo Y100i 长续航版 5G（brands/vivo_cn.md:797）
- `V2317A`：vivo vivo Y12（brands/vivo_cn.md:487）；vivo vivo Y33t（brands/vivo_cn.md:547）
- `V2318A`：vivo vivo Y36 5G / vivo Y36m 5G / vivo Y36i 5G / vivo Y36s 5G（brands/vivo_cn.md:559）；vivo vivo G2（brands/vivo_cn.md:967）
- `V2354A`：vivo vivo Y37 Pro 5G（brands/vivo_cn.md:575）；vivo vivo Y100+ 5G / vivo Y200i 5G / vivo Y200+ 5G（brands/vivo_cn.md:801）
- `V2443A`：vivo vivo Y37t 5G / vivo Y37+ 5G / vivo Y50 5G / vivo Y50i 5G / vivo Y50e 5G / vivo Y50s 5G（brands/vivo_cn.md:591）；vivo vivo G3（brands/vivo_cn.md:971）
- `V2531A`：vivo vivo Y500i / vivo Y500s / vivo Y6 / vivo Y6a（brands/vivo_cn.md:849）；vivo vivo G5i / vivo G5z（brands/vivo_cn.md:975）
- `VER-AN10`：HONOR 荣耀 Magic V2（brands/honor_cn.md:71）；HONOR 荣耀 Magic V2 至臻版（brands/honor_cn.md:75）；HONOR 荣耀 Magic V2 RSR 保时捷设计（brands/honor_cn.md:79）
- `VNE-AN40`：HONOR 荣耀 Play6C 5G（brands/honor_cn.md:809）；HONOR 荣耀畅玩 30M 5G（brands/honor_cn.md:1047）
- `WEB-W00`：HUAWEI HUAWEI MatePad Pro 13.2 英寸 2025 Wi-Fi 版（brands/huawei_cn.md:1314）；HUAWEI 华为擎云 C9 13.2 英寸 Wi-Fi 版（brands/huawei_cn.md:1632）
- `ZTE 8012N`：ZTE 中兴 Blade 20 5G（brands/zte_cn.md:167）；ZTE 中兴 Blade V2021 5G（brands/zte_cn.md:179）
- `ZTE 9000N`：ZTE 中兴天机 Axon 11 SE 5G（brands/zte_cn.md:39）；ZTE 中兴 Blade V2020（brands/zte_cn.md:171）
- `ZTE 9041N`：ZTE 中兴远航 30 Pro+（brands/zte_cn.md:227）；ZTE 中兴 V70（brands/zte_cn.md:291）
- `ZTE A2022P`：ZTE 中兴 Axon 30 Ultra 5G（brands/zte_cn.md:61）；ZTE 中兴天机 A31 Ultra 5G（brands/zte_cn.md:73）
- `ZTE A2023`：ZTE 中兴 Axon 40 Pro（brands/zte_cn.md:77）；ZTE 中兴天机 A41 Pro（brands/zte_cn.md:99）
- `ZTE A2023P`：ZTE 中兴 Axon 40 Ultra（brands/zte_cn.md:81）；ZTE 中兴天机 A41 Ultra 至尊版（brands/zte_cn.md:107）
- `ZTE A2122H`：ZTE 中兴天机 A31 5G（brands/zte_cn.md:65）；ZTE 中兴 S30 Pro 5G（brands/zte_cn.md:115）

## 已排除条目示例

- HONOR 荣耀 MagicBook X 14 i3（`NBR-WAI9`，brands/honor_cn.md:1687）
- HONOR 荣耀 MagicBook X 14 i5（`NBR-WAH9`，brands/honor_cn.md:1689）
- HONOR 荣耀 MagicBook X 14 2022 i5 (11 代酷睿)（`NDR-WFH`，brands/honor_cn.md:1693）
- HONOR 荣耀 MagicBook X 14 2022 i5 (12 代酷睿)（`FRI-F56`，brands/honor_cn.md:1697）
- HONOR 荣耀 MagicBook X 14 2023 i3 (8GB+256GB)（`FRI-F38`，brands/honor_cn.md:1701）
- HONOR 荣耀 MagicBook X 14 2023 i5 (16GB+512GB)（`FRI-F56`，brands/honor_cn.md:1703）
- HONOR 荣耀 MagicBook X 14 2023 i5 (16GB+1TB)（`FRI-F561`，brands/honor_cn.md:1705）
- HONOR 荣耀 MagicBook X 14 Pro 2023 i5 (16GB+512GB)（`FRI-G56`，brands/honor_cn.md:1709）
- HONOR 荣耀 MagicBook X 14 Pro 2023 i5 (16GB+1TB)（`FRI-G561`，brands/honor_cn.md:1711）
- HONOR 荣耀 MagicBook X 14 Pro 2023 R7 (16GB+512GB)（`FRI-H76`，brands/honor_cn.md:1715）
- HONOR 荣耀笔记本 X14 2024 i5 (16GB+512GB)（`FRI-G56`，brands/honor_cn.md:1719）
- HONOR 荣耀笔记本 X14 2024 i5 (16GB+1TB)（`FRI-G561`，brands/honor_cn.md:1721）
- HONOR 荣耀笔记本 X14 2024 R5 (16GB+512GB)（`FRI-H56`，brands/honor_cn.md:1725）
- HONOR 荣耀笔记本 X14 2025（`FRG-X`，brands/honor_cn.md:1729）
- HONOR 荣耀笔记本 X14 2025 战斗版（`GDG-X`，brands/honor_cn.md:1733）
- HONOR 荣耀笔记本 X14 2026（`FRG-X`，brands/honor_cn.md:1737）
- HONOR 荣耀笔记本 X14 2026 战斗版 / 荣耀笔记本 X14 Plus 2026 战斗版（`LVD-X`，brands/honor_cn.md:1741）
- HONOR 荣耀笔记本 X14 锐龙版 2026（`GDH-X`，brands/honor_cn.md:1745）
- HONOR 荣耀笔记本 X14 Plus 2024 R7 (16GB+512GB)（`FMI-76`，brands/honor_cn.md:1749）
- HONOR 荣耀笔记本 X14 Plus 2024 R7 (32GB+1TB)（`FMI-721`，brands/honor_cn.md:1751）
- HONOR 荣耀笔记本 X14 Plus 2025（`FRB-X`，brands/honor_cn.md:1755）
- HONOR 荣耀笔记本 X14 Plus 2026（`LVC-X`，brands/honor_cn.md:1759）
- HONOR 荣耀 MagicBook X 15 i3（`BBR-WAI9`，brands/honor_cn.md:1763）
- HONOR 荣耀 MagicBook X 15 i5（`BBR-WAH9`，brands/honor_cn.md:1765）
- HONOR 荣耀 MagicBook X 15 2022 i5 (8GB+512GB)（`BDR-WDH`，brands/honor_cn.md:1769）
- HONOR 荣耀 MagicBook X 15 2022 i5 (16GB+512GB)（`BDR-WFH`，brands/honor_cn.md:1771）
- HONOR 荣耀 MagicBook X 16 2022 i5（`BRN-F56`，brands/honor_cn.md:1775）
- HONOR 荣耀 MagicBook X 16 2023 i5 (16GB+512GB)（`BRN-F56`，brands/honor_cn.md:1779）
- HONOR 荣耀 MagicBook X 16 2023 i5 (16GB+1TB)（`BRN-F561`，brands/honor_cn.md:1781）
- HONOR 荣耀 MagicBook X 16 Pro 2023 i5 (16GB+512GB)（`BRN-G56`，brands/honor_cn.md:1785）
- HONOR 荣耀 MagicBook X 16 Pro 2023 i5 (16GB+1TB)（`BRN-G561`，brands/honor_cn.md:1787）
- HONOR 荣耀 MagicBook X 16 Pro 2023 R7 (16GB+512GB)（`BRN-H76`，brands/honor_cn.md:1791）
- HONOR 荣耀笔记本 X16 2024 i5 (16GB+512GB)（`BRN-G56`，brands/honor_cn.md:1795）
- HONOR 荣耀笔记本 X16 2024 i5 (16GB+1TB)（`BRN-G561`，brands/honor_cn.md:1797）
- HONOR 荣耀笔记本 X16 2024 R5 (16GB+512GB)（`BRN-H56`，brands/honor_cn.md:1801）
- HONOR 荣耀笔记本 X16 2025 i5 (8GB+512GB)（`BRG-585`，brands/honor_cn.md:1805）
- HONOR 荣耀笔记本 X16 2025 i5 (16GB+512GB)（`BRG-565`，brands/honor_cn.md:1807）
- HONOR 荣耀笔记本 X16 2025 i5 (16GB+1TB)（`BRG-561`，brands/honor_cn.md:1809）
- HONOR 荣耀笔记本 X16 2025 i7 (16GB+512GB)（`BRG-765`，brands/honor_cn.md:1811）
- HONOR 荣耀笔记本 X16 2025 i7 (16GB+1TB)（`BRG-761`，brands/honor_cn.md:1813）
