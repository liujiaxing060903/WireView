import importlib.util
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("cursor_analysis", Path(__file__).with_name("analyze-cursor-latency.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    log = root / "performance-test.log"
    surface = root / "surface.txt"
    log.write_text("Cursor diagnostics: variant=current-output-thread\n" + "\n".join(
        f"Cursor frame marker: framePtsUs={pts} | motion={motion} | observedHostUs={observed} | surfaceWantedNs={wanted} | androidMinusHostUs=300000 | pollGapMs=1 | clockRttMs=2"
        for pts, motion, observed, wanted in [(1,1,2000,998000000), (2,1,2000,999000000),
                                            (3,1,2000,999100000), (4,2,3000,999200000)]), encoding="utf-8")
    surface.write_text("999000000 322000000 0\n999100000 323000000 0\n999200000 325000000 0\n", encoding="utf-8")
    result = module.analyze(log, surface)
    assert result["samples"] == 2 and result["compositorMatchedMarkers"] == 3, result
    assert result["observedToPresentEstimateMs"]["mean"] == 21.0, result
    assert module.analyze(log)["samples"] == 0
    log.write_text("Cursor diagnostics: variant=v1.1.20\nCursor latency sample: framePtsUs=999000 | motion=1 | observedToRenderEstimateMs=697.000 | pollGapMs=1 | clockRttMs=2\n", encoding="utf-8")
    assert module.analyze(log, surface)["samples"] == 0
    recovered = module.analyze(log, surface, True)
    assert recovered["observedToPresentEstimateMs"]["mean"] == 20.0, recovered
print("Compositor cursor latency analysis tests passed")
