"""Add the same opt-in cursor timing probe to current and exported v1.1.20 sources."""
from pathlib import Path
import shutil
import sys

workspace = Path(__file__).resolve().parents[1]
target = Path(sys.argv[1]).resolve()
is_old = target != workspace
if 'on_cursor_sample;' in (target / 'windows/host/src/windows_video.hpp').read_text(encoding='utf-8-sig'):
    print(f"Cursor timing is already instrumented: {target}")
    sys.exit(0)

def edit(relative, before, after):
    path = target / relative
    text = path.read_text(encoding="utf-8-sig")
    if text.count(before) != 1:
        raise RuntimeError(f"Expected one instrumentation insertion point: {relative}: {before[:60]!r}")
    path.write_text(text.replace(before, after), encoding="utf-8", newline="\n")

if is_old:
    for name in ["cursor_latency.hpp", "session_performance_log.hpp", "frame_pacer.hpp"]:
        shutil.copy2(workspace / "windows/host/src" / name, target / "windows/host/src" / name)
    shutil.copy2(workspace / "android/app/src/main/java/com/wireview/receiver/SurfaceFrameMarker.java",
                 target / "android/app/src/main/java/com/wireview/receiver/SurfaceFrameMarker.java")

edit("windows/host/src/windows_video.hpp", "    std::function<bool()> consume_keyframe_request;",
     "    std::function<bool()> consume_keyframe_request;\n"
     "    // Optional diagnostic correlation; does not change cursor composition.\n"
     "    std::function<void(std::uint64_t, int, int)> on_cursor_sample;")
edit("windows/host/src/desktop_encoder.cpp",
     "            cursor_compositor->compose(frames->texture(), frames->pointer());",
     "            cursor_compositor->compose(frames->texture(), frames->pointer());\n"
     "            if (callbacks.on_cursor_sample && frames->pointer().visible) {\n"
     "                callbacks.on_cursor_sample(stream_timestamp_base +\n"
     "                    static_cast<std::uint64_t>((static_cast<LONGLONG>(submitted) * frame_duration) / 10),\n"
     "                    frames->pointer().x, frames->pointer().y);\n"
     "            }")

transport = "windows/host/src/development_transport.cpp"
edit(transport, "#include <WS2tcpip.h>",
     '#include <WS2tcpip.h>\n\n#include "cursor_latency.hpp"\n#include "frame_pacer.hpp"' +
     ('\n#include "session_performance_log.hpp"' if is_old else ''))
edit(transport, "class WinsockSession {", '''[[nodiscard]] bool cursor_diagnostics_enabled() {
    if (GetEnvironmentVariableA("WIREVIEW_CURSOR_DIAGNOSTICS", nullptr, 0) > 0) { return true; }
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    return length != 0U && std::filesystem::exists(
        std::filesystem::path(executable.data()).parent_path() / L"cursor-diagnostics.enabled");
}

class WinsockSession {''')
edit(transport, "    InputReceiveStats& stats,\n    DevelopmentStatusPublisher* status) {",
     "    InputReceiveStats& stats,\n    DevelopmentStatusPublisher* status,\n"
     "    CursorLatencyMeasurements& cursor_timings) {")
current_transport = (workspace / transport).read_text(encoding="utf-8")
handler_begin = current_transport.index("                if (payload.size() == 20U && payload[0] == 1U && payload[1] == 8U) {")
handler_end = current_transport.index("                } else if (payload.size() == 16U", handler_begin)
marker_handler = current_transport[handler_begin:handler_end]
edit(transport, "                if (payload.size() == 16U && payload[0] == 1U && payload[1] == 1U) {",
     marker_handler + "                } else if (payload.size() == 16U && payload[0] == 1U && payload[1] == 1U) {")
probe = r'''    CursorLatencyMeasurements cursor_timings;
    const bool cursor_diagnostics = cursor_diagnostics_enabled();
    std::jthread cursor_observer;
    if (cursor_diagnostics) {
        std::cout << "Cursor diagnostics: variant=VARIANT | source=Windows observed motion"
                     " | samplePeriodMs=1 | actualPresentSource=SurfaceFlinger; not optical latency\n";
        cursor_observer = std::jthread([&](std::stop_token stop) {
            FramePacer timer;
            while (!stop.stop_requested()) {
                POINT point{};
                if (GetCursorPos(&point)) {
                    cursor_timings.observe(point.x, point.y, monotonic_micros());
                }
                timer.wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
            }
        });
    }
'''.replace("VARIANT", "v1.1.20" if is_old else "current-output-thread")
if is_old:
    probe = r'''    std::optional<SessionPerformanceLog> comparison_log;
    if (cursor_diagnostics_enabled()) {
        std::array<wchar_t, 32768> directory{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), static_cast<DWORD>(directory.size()));
        if (length == 0U || length >= directory.size()) {
            throw std::runtime_error("Cannot locate cursor diagnostic log directory");
        }
        comparison_log.emplace(std::filesystem::path(directory.data()) / L"WireView" / L"logs", GetCurrentProcessId());
        std::cout << "Performance log: " << comparison_log->path().string() << '\n';
    }
''' + probe
edit(transport, "    std::uint64_t sequence = 2U;\n    std::atomic_bool keyframe_requested{false};",
     probe + "    std::uint64_t sequence = 2U;\n    std::atomic_bool keyframe_requested{false};")
edit(transport, "    const DesktopStreamCallbacks callbacks{", "    DesktopStreamCallbacks callbacks{")
cursor_callback = "    if (cursor_diagnostics) {\n" \
     + "        callbacks.on_cursor_sample = [&](std::uint64_t pts, int x, int y) {\n" \
     + "            cursor_timings.correlate(pts, input_target.left + x, input_target.top + y);\n" \
     + "        };\n    }\n"
if is_old:
    edit(transport, "    WindowsInputInjector injector(input_target);",
         "    WindowsInputInjector injector(input_target);\n" + cursor_callback)
else:
    edit(transport, '    std::cout << "Capture target: "', cursor_callback + '\n    std::cout << "Capture target: "')
edit(transport, "input_stats, status);", "input_stats, status, cursor_timings);")

decoder = "android/app/src/main/java/com/wireview/receiver/VideoDecoder.java"
edit(decoder, "        void onFirstFrameRendered(long presentationTimeUs, long startupMillis);",
     "        void onFirstFrameRendered(long presentationTimeUs, long startupMillis);\n\n"
     "        void onFrameSurfaceTarget(long presentationTimeUs, long wantedNanos);")
if not is_old:
    raise RuntimeError("Current source already contains the diagnostic integration")
edit(decoder, "                codec.releaseOutputBuffer(outputIndex, true);",
     "                codec.releaseOutputBuffer(outputIndex, true);\n"
     "                if (BuildConfig.CURSOR_DIAGNOSTICS) {\n"
     "                    listener.onFrameSurfaceTarget(info.presentationTimeUs, info.presentationTimeUs * 1_000L);\n"
     "                }")
activity = "android/app/src/main/java/com/wireview/receiver/MainActivity.java"
edit(activity, "    @Override\n    public void onDecoderHealth(",
     "    @Override\n    public void onFrameSurfaceTarget(long presentationTimeUs, long wantedNanos) {\n"
     "        connection.sendTelemetry(SurfaceFrameMarker.encode(presentationTimeUs, wantedNanos));\n"
     "    }\n\n    @Override\n    public void onDecoderHealth(")
edit("android/app/build.gradle.kts", "    defaultConfig {",
     '    buildFeatures { buildConfig = true }\n\n    defaultConfig {\n'
     '        buildConfigField("boolean", "CURSOR_DIAGNOSTICS",\n'
     '            providers.gradleProperty("wireviewCursorDiagnostics").orElse("false").get())')

if is_old:
    shutil.copy2(workspace / "scripts/build-android.ps1", target / "scripts/build-android.ps1")
print(f"Instrumented {'v1.1.20' if is_old else 'current'}: {target}")
