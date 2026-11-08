#include "input_injector.hpp"

#include "wireview/input.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace wireview::host {
namespace {

using wireview::protocol::InputAction;
using wireview::protocol::InputRecord;
using wireview::protocol::InputTool;

[[nodiscard]] LONG map_coordinate(
    std::uint16_t normalized,
    int origin,
    int extent) noexcept {
    if (extent <= 1) {
        return static_cast<LONG>(origin);
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(normalized) * static_cast<std::uint64_t>(extent - 1);
    return static_cast<LONG>(origin + static_cast<int>(scaled / 65'535U));
}

[[nodiscard]] POINT map_to_target_screen(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    const DesktopCaptureArea& target) noexcept {
    return POINT{
        map_coordinate(normalized_x, target.left, static_cast<int>(target.width)),
        map_coordinate(normalized_y, target.top, static_cast<int>(target.height)),
    };
}

[[nodiscard]] POINT map_to_synthetic_pointer_location(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    const DesktopCaptureArea& target) noexcept {
    POINT location = map_to_target_screen(normalized_x, normalized_y, target);
    // InjectSyntheticPointerInput defines ptPixelLocation relative to the
    // virtual desktop's top-left corner, unlike the global screen coordinates
    // returned by the target-display mapping above. This distinction matters
    // whenever a display is placed left of or above the primary monitor.
    location.x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
    location.y -= GetSystemMetrics(SM_YVIRTUALSCREEN);
    return location;
}

[[nodiscard]] LONG map_to_virtual_desktop_axis(int coordinate, int origin, int extent) noexcept {
    if (extent <= 1) {
        return 0;
    }
    const int relative = std::clamp(coordinate - origin, 0, extent - 1);
    return static_cast<LONG>(
        (static_cast<std::uint64_t>(relative) * 65'535U) /
        static_cast<std::uint64_t>(extent - 1));
}

[[nodiscard]] POINTER_FLAGS pointer_flags(InputAction action, bool primary = true) noexcept {
    POINTER_FLAGS flags = POINTER_FLAG_NONE;
    switch (action) {
        case InputAction::down:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                    POINTER_FLAG_DOWN | POINTER_FLAG_CONFIDENCE;
            break;
        case InputAction::move:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                    POINTER_FLAG_UPDATE | POINTER_FLAG_CONFIDENCE;
            break;
        case InputAction::up:
            flags = POINTER_FLAG_UP;
            break;
        case InputAction::cancel:
            flags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
            break;
        case InputAction::hover:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE |
                    POINTER_FLAG_CONFIDENCE;
            break;
    }
    return primary ? flags | POINTER_FLAG_PRIMARY : flags;
}

[[nodiscard]] UINT32 map_pressure(std::uint16_t pressure) noexcept {
    return static_cast<UINT32>(
        (static_cast<std::uint64_t>(pressure) * 1'024U + 32'767U) / 65'535U);
}

void report_injection_failure(const char* operation) {
    std::cerr << operation << " failed: " << GetLastError() << '\n';
}

struct SyntheticPointerApi {
    using CreateDevice = HSYNTHETICPOINTERDEVICE(WINAPI*)(
        POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using InjectInput = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, POINTER_TYPE_INFO*, UINT32);
    using DestroyDevice = void(WINAPI*)(HSYNTHETICPOINTERDEVICE);

    CreateDevice create_device{};
    InjectInput inject_input{};
    DestroyDevice destroy_device{};

    [[nodiscard]] bool available() const noexcept {
        return create_device != nullptr && inject_input != nullptr && destroy_device != nullptr;
    }
};

const SyntheticPointerApi& synthetic_pointer_api() {
    static const SyntheticPointerApi api = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            return SyntheticPointerApi{};
        }
        return SyntheticPointerApi{
            .create_device = reinterpret_cast<SyntheticPointerApi::CreateDevice>(
                GetProcAddress(user32, "CreateSyntheticPointerDevice")),
            .inject_input = reinterpret_cast<SyntheticPointerApi::InjectInput>(
                GetProcAddress(user32, "InjectSyntheticPointerInput")),
            .destroy_device = reinterpret_cast<SyntheticPointerApi::DestroyDevice>(
                GetProcAddress(user32, "DestroySyntheticPointerDevice")),
        };
    }();
    return api;
}

}  // namespace

WindowsInputInjector::WindowsInputInjector(DesktopCaptureArea target)
    : target_(target) {
    if (target_.width == 0U || target_.height == 0U) {
        target_.left = 0;
        target_.top = 0;
        target_.width = static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        target_.height = static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    }
}

WindowsInputInjector::~WindowsInputInjector() {
    const auto& api = synthetic_pointer_api();
    if (!api.available()) {
        return;
    }
    if (touch_device_ != nullptr) {
        api.destroy_device(
            static_cast<HSYNTHETICPOINTERDEVICE>(touch_device_));
    }
    if (pen_device_ != nullptr) {
        api.destroy_device(
            static_cast<HSYNTHETICPOINTERDEVICE>(pen_device_));
    }
}

bool WindowsInputInjector::inject_batch(
    const wireview::protocol::InputBatch& batch) {
    bool success = true;
    bool has_touch_records = false;
    for (const InputRecord& record : batch.records) {
        switch (record.tool) {
            case InputTool::finger:
                ++metrics_.finger_records;
                has_touch_records = true;
                break;
            case InputTool::stylus:
            case InputTool::eraser:
            {
                const auto started_at = std::chrono::steady_clock::now();
                ++metrics_.pen_records;
                const bool record_success = inject_pen_record(
                    static_cast<std::uint8_t>(record.action),
                    record.tool == InputTool::eraser,
                    record.pointer_id,
                    record.normalized_x,
                    record.normalized_y,
                    record.pressure,
                    record.tilt_x_hundredths,
                    record.tilt_y_hundredths,
                    record.buttons);
                const auto elapsed_micros = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started_at).count());
                ++metrics_.attempts;
                metrics_.total_micros += elapsed_micros;
                metrics_.maximum_micros = std::max(metrics_.maximum_micros, elapsed_micros);
                if (record_success) {
                    ++metrics_.successful;
                }
                success = record_success && success;
                break;
            }
            case InputTool::mouse:
            {
                const auto started_at = std::chrono::steady_clock::now();
                ++metrics_.mouse_records;
                const bool record_success = inject_mouse_record(
                    static_cast<std::uint8_t>(record.action),
                    record.normalized_x,
                    record.normalized_y);
                const auto elapsed_micros = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started_at).count());
                ++metrics_.attempts;
                metrics_.total_micros += elapsed_micros;
                metrics_.maximum_micros = std::max(metrics_.maximum_micros, elapsed_micros);
                if (record_success) {
                    ++metrics_.successful;
                }
                success = record_success && success;
                break;
            }
        }
    }
    if (has_touch_records) {
        const auto started_at = std::chrono::steady_clock::now();
        const bool touch_success = inject_touch_batch(batch);
        const auto elapsed_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_at).count());
        ++metrics_.attempts;
        metrics_.total_micros += elapsed_micros;
        metrics_.maximum_micros = std::max(metrics_.maximum_micros, elapsed_micros);
        if (touch_success) {
            ++metrics_.successful;
        }
        success = touch_success && success;
    }
    injected_records_ += batch.records.size();
    return success;
}

std::uint64_t WindowsInputInjector::injected_records() const noexcept {
    return injected_records_;
}

const InputInjectionMetrics& WindowsInputInjector::metrics() const noexcept {
    return metrics_;
}

bool WindowsInputInjector::inject_touch_batch(const wireview::protocol::InputBatch& batch) {
    const auto& api = synthetic_pointer_api();
    if (!api.available()) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        report_injection_failure("Synthetic pointer input is unavailable on this Windows version");
        return false;
    }

    // Stage the whole batch against a copy of the contact map: the member
    // state must stay untouched until the injection below succeeds. The old
    // write-as-you-go flow aborted midway (contact limit, device creation,
    // rejected frame) with half of the batch already applied, and those
    // stale contacts were re-sent as ghost touches on every later batch.
    std::map<std::uint16_t, TouchContact> staged_touches = active_touches_;
    std::vector<std::uint16_t> released_pointer_ids;
    std::optional<std::uint16_t> frame_primary = primary_touch_id_;
    for (const InputRecord& record : batch.records) {
        if (record.tool != InputTool::finger) {
            continue;
        }
        const auto action = record.action;
        if (action == InputAction::hover) {
            // A hover for a contact still held down is an Android
            // tool-switch transition: treat it as a plain move so the
            // contact does not freeze at stale coordinates. Hovers for
            // pointers that are unknown or already released this batch stay
            // skipped.
            const auto held = staged_touches.find(record.pointer_id);
            if (held != staged_touches.end() &&
                held->second.action != static_cast<std::uint8_t>(InputAction::up) &&
                held->second.action != static_cast<std::uint8_t>(InputAction::cancel)) {
                held->second = TouchContact{
                    .normalized_x = record.normalized_x,
                    .normalized_y = record.normalized_y,
                    .pressure = record.pressure,
                    .action = static_cast<std::uint8_t>(InputAction::move),
                };
            }
            continue;
        }
        TouchContact contact{
            .normalized_x = record.normalized_x,
            .normalized_y = record.normalized_y,
            .pressure = record.pressure,
            .action = static_cast<std::uint8_t>(action),
        };
        if ((action == InputAction::down || action == InputAction::move) &&
            staged_touches.size() >= 10U &&
            !staged_touches.contains(record.pointer_id)) {
            std::cerr << "WireView touch contact limit exceeded\n";
            return false;
        }
        staged_touches[record.pointer_id] = contact;
        if (!frame_primary) {
            frame_primary = record.pointer_id;
        }
        if (action == InputAction::up || action == InputAction::cancel) {
            released_pointer_ids.push_back(record.pointer_id);
        }
    }

    if (staged_touches.empty()) {
        return true;
    }
    if (touch_device_ == nullptr) {
        touch_device_ = api.create_device(
            PT_TOUCH, 10, POINTER_FEEDBACK_DEFAULT);
        if (touch_device_ == nullptr) {
            report_injection_failure("CreateSyntheticPointerDevice(PT_TOUCH)");
            return false;
        }
    }

    const auto make_touch_info = [this](
        std::uint16_t pointer_id, const TouchContact& contact, InputAction action,
        bool primary) {
        const POINT location = map_to_synthetic_pointer_location(
            contact.normalized_x, contact.normalized_y, target_);
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        info.touchInfo.pointerInfo.pointerType = PT_TOUCH;
        info.touchInfo.pointerInfo.pointerId = pointer_id;
        info.touchInfo.pointerInfo.pointerFlags = pointer_flags(action, primary);
        info.touchInfo.pointerInfo.ptPixelLocation = location;
        info.touchInfo.touchFlags = TOUCH_FLAG_NONE;
        info.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        info.touchInfo.rcContact = RECT{
            location.x - 2, location.y - 2, location.x + 2, location.y + 2};
        info.touchInfo.pressure = map_pressure(contact.pressure);
        return info;
    };

    std::vector<POINTER_TYPE_INFO> infos;
    infos.reserve(staged_touches.size());
    for (const auto& [pointer_id, contact] : staged_touches) {
        infos.push_back(make_touch_info(
            pointer_id, contact, static_cast<InputAction>(contact.action),
            frame_primary && *frame_primary == pointer_id));
    }
    if (!api.inject_input(
            static_cast<HSYNTHETICPOINTERDEVICE>(touch_device_),
            infos.data(),
            static_cast<UINT32>(infos.size()))) {
        report_injection_failure("InjectSyntheticPointerInput(touch)");
        // The frame was rejected wholesale, so the OS still holds every
        // contact from the last successful batch. Push one last best-effort
        // UP frame for each of them, then drop the map: an empty map cannot
        // breed ghost touches on later batches, unlike leftover state.
        std::vector<POINTER_TYPE_INFO> cancels;
        cancels.reserve(active_touches_.size());
        for (const auto& [pointer_id, contact] : active_touches_) {
            cancels.push_back(make_touch_info(
                pointer_id, contact, InputAction::up,
                primary_touch_id_ && *primary_touch_id_ == pointer_id));
        }
        if (!cancels.empty()) {
            (void)api.inject_input(
                static_cast<HSYNTHETICPOINTERDEVICE>(touch_device_),
                cancels.data(),
                static_cast<UINT32>(cancels.size()));
        }
        active_touches_.clear();
        primary_touch_id_.reset();
        return false;
    }

    // Commit on success only: publish the staged map, retire released
    // contacts, and normalize the survivors to move for the next batch.
    active_touches_ = std::move(staged_touches);
    primary_touch_id_ = frame_primary;
    for (const std::uint16_t pointer_id : released_pointer_ids) {
        active_touches_.erase(pointer_id);
    }
    if (primary_touch_id_ && !active_touches_.contains(*primary_touch_id_)) {
        primary_touch_id_ = active_touches_.empty()
            ? std::nullopt
            : std::optional<std::uint16_t>(active_touches_.begin()->first);
    }
    for (auto& [pointer_id, contact] : active_touches_) {
        contact.action = static_cast<std::uint8_t>(InputAction::move);
    }
    return true;
}

bool WindowsInputInjector::inject_pen_record(
    std::uint8_t action_value,
    bool eraser,
    std::uint16_t pointer_id,
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    std::uint16_t pressure,
    std::int16_t tilt_x_hundredths,
    std::int16_t tilt_y_hundredths,
    std::uint16_t buttons) {
    const auto& api = synthetic_pointer_api();
    if (!api.available()) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        report_injection_failure("Synthetic pointer input is unavailable on this Windows version");
        return false;
    }
    if (pen_device_ == nullptr) {
        pen_device_ = api.create_device(
            PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        if (pen_device_ == nullptr) {
            report_injection_failure("CreateSyntheticPointerDevice(PT_PEN)");
            return false;
        }
    }

    const auto action = static_cast<InputAction>(action_value);
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo.pointerType = PT_PEN;
    info.penInfo.pointerInfo.pointerId = pointer_id;
    info.penInfo.pointerInfo.pointerFlags = pointer_flags(action);
    info.penInfo.pointerInfo.ptPixelLocation =
        map_to_synthetic_pointer_location(normalized_x, normalized_y, target_);
    info.penInfo.penFlags = PEN_FLAG_NONE;
    if (eraser) {
        info.penInfo.penFlags |= PEN_FLAG_ERASER | PEN_FLAG_INVERTED;
    }
    if (buttons != 0U) {
        info.penInfo.penFlags |= PEN_FLAG_BARREL;
    }
    info.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    info.penInfo.pressure = map_pressure(pressure);
    info.penInfo.tiltX = std::clamp<INT32>(tilt_x_hundredths / 100, -90, 90);
    info.penInfo.tiltY = std::clamp<INT32>(tilt_y_hundredths / 100, -90, 90);
    if (!api.inject_input(
            static_cast<HSYNTHETICPOINTERDEVICE>(pen_device_), &info, 1)) {
        report_injection_failure("InjectSyntheticPointerInput(pen)");
        return false;
    }
    return true;
}

bool WindowsInputInjector::inject_mouse_record(
    std::uint8_t action_value,
    std::uint16_t normalized_x,
    std::uint16_t normalized_y) {
    const auto action = static_cast<InputAction>(action_value);
    const POINT location = map_to_target_screen(normalized_x, normalized_y, target_);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = map_to_virtual_desktop_axis(
        location.x,
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN));
    input.mi.dy = map_to_virtual_desktop_axis(
        location.y,
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                       MOUSEEVENTF_MOVE;
    if (action == InputAction::down) {
        input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    } else if (action == InputAction::up || action == InputAction::cancel) {
        input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    }
    if (SendInput(1, &input, sizeof(input)) != 1U) {
        report_injection_failure("SendInput(mouse)");
        return false;
    }
    return true;
}

}  // namespace wireview::host
