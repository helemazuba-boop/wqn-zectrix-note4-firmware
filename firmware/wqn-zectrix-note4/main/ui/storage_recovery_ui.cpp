#include "ui/storage_recovery_ui.h"

#include <cstdio>

#include "button_input.h"
#include "esp_system.h"
#include "services/storage_recovery_service.h"
#include "ui/storage_recovery_model.h"
#include "ui/ui_internal.h"

namespace device_ui_internal {
namespace {
using Model = StorageRecoveryModel;
using Page = Model::Page;
using Action = Model::Action;
using Policy = wqn::runtime::StorageRecoveryResetPolicy;
constexpr char kTag[] = "storage_recovery_ui";

void BuildRecoveryFrame(const Model& model, esp_err_t boot_error, wqn::UiFrame* frame)
{
    frame->screen = wqn::UiScreen::kProvisioning;
    frame->prefer_full_refresh = true;
    frame->lines.clear();
    auto line = [frame](const char* text, wqn::UiTextStyle style = wqn::UiTextStyle::kBody) {
        frame->lines.push_back({style, text});
    };
    char text[100];
    line(model.preservation_only() ? "数据保全" : "存储恢复", wqn::UiTextStyle::kTitle);
    if (model.page() == Page::kOverview) {
        if (model.preservation_only()) {
            line("常规启动已暂停。未初始化或挂载业务存储。", wqn::UiTextStyle::kWrappedBody);
            line("松开上键，再选择操作。");
            if (boot_error != ESP_OK) line("启动按键检查异常，已保留数据。", wqn::UiTextStyle::kWarning);
            line(model.selection() == 0 ? "> 保全原始镜像到电脑" : "  保全原始镜像到电脑");
            line(model.selection() == 1 ? "> 重启正常启动" : "  重启正常启动");
            return;
        }
        std::snprintf(text, sizeof(text), "启动错误：%s", esp_err_to_name(boot_error));
        line(text, wqn::UiTextStyle::kWarning);
        if (model.snapshot().reset_policy == Policy::kNewerFirmwareRequired)
            line("数据版本较新，请使用更新固件。");
        else if (model.snapshot().reset_policy == Policy::kAlreadyPending)
            line("已有重置请求；重启将继续清除。");
        else if (!model.CanReset()) line("诊断未就绪，暂不可清除数据。");
        static constexpr const char* items[] = {
            "导出诊断到串口", "重启并重试", "保全数据到电脑", "清除本地数据"};
        for (unsigned index = 0; index < 4; ++index) {
            std::snprintf(text, sizeof(text), "%s%s%s", model.selection() == index ? "> " : "  ",
                items[index], index == 3 && !model.CanReset() ? "（不可用）" : "");
            line(text, model.selection() == index ? wqn::UiTextStyle::kSelected : wqn::UiTextStyle::kBody);
        }
    } else if (model.page() == Page::kConfirmReset) {
        line("确认清除全部本地数据？", wqn::UiTextStyle::kWarning);
        line("将删除 WiFi、配对、学习包和未同步记录。", wqn::UiTextStyle::kWrappedBody);
        line("操作无法撤销，需要重新配网。");
        line("按住确认满 2 秒，再松开。");
        line("按上键取消。");
    } else if (model.page() == Page::kConfirmRestart) {
        line(model.preservation_only() ? "恢复常规启动？" : "确认重启并重新检查存储？");
        line("若先前已确认重置，重启将继续清除数据。", wqn::UiTextStyle::kWrappedBody);
        line("短按确认重启，按上键返回。");
    } else if (model.page() == Page::kPreservation) {
        line(model.export_raw() ? "  学习记录与上下文" : "> 学习记录与上下文");
        line(model.export_raw() ? "> 完整主 Flash 原始镜像" : "  完整主 Flash 原始镜像");
        line("学习数据不可读取时，可选择原始镜像。", wqn::UiTextStyle::kWrappedBody);
        line("下键切换，确认继续，上键返回。");
    } else if (model.page() == Page::kConfirmExport) {
        line(model.export_raw() ? "导出原始主 Flash 镜像？" : "导出学习记录与上下文？", wqn::UiTextStyle::kWarning);
        line(model.export_raw() ? "镜像可能包含 WiFi 与配对凭据，请保存在可信电脑。" :
            "内容包含本机学习记录，请保存在可信电脑。", wqn::UiTextStyle::kWrappedBody);
        line("连接 USB，并启动电脑接收工具。");
        line("按住确认满 2 秒，再松开。上键取消。");
    } else if (model.page() == Page::kExportResult) {
        const auto& result = model.export_result();
        if (result.commit_acknowledged) {
            line(result.source_complete ? "电脑已校验并保存证据。" : "电脑已保存未完成的证据，源数据仍需检查。", wqn::UiTextStyle::kWrappedBody);
            if (model.error() != ESP_OK) line("设备结束流程未完成，请保留接收结果。");
            std::snprintf(text, sizeof(text), "保存校验：%02x%02x%02x%02x",
                result.manifest_sha256[0], result.manifest_sha256[1], result.manifest_sha256[2], result.manifest_sha256[3]); line(text);
        } else line("导出未确认，电脑可能保留部分数据。", wqn::UiTextStyle::kWrappedBody);
        line("本地数据未清除。重启可恢复 USB 日志。");
        line("短按确认返回。");
    } else if (model.page() == Page::kDiagnostics) {
        std::snprintf(text, sizeof(text), "读取结果：%s", esp_err_to_name(model.error())); line(text);
        std::snprintf(text, sizeof(text), "数据版本：%lu / 固件支持：%lu",
            static_cast<unsigned long>(model.snapshot().observed_generation),
            static_cast<unsigned long>(wqn::runtime::kStorageSchemaGeneration)); line(text);
        line("诊断状态已输出串口，请保存日志。");
        line("本次诊断不含业务数据或凭据。");
        line("短按确认返回。");
    } else if (model.page() == Page::kBusy) {
        if (model.exporting()) {
            line(model.export_cancel_requested() ? "正在停止导出…" : "等待电脑接收或正在导出…");
            std::snprintf(text, sizeof(text), "电脑已落盘：%lu KiB", static_cast<unsigned long>(model.exported_bytes()/1024)); line(text);
            line("上键可停止；请保留电脑接收结果。");
        } else line(model.operation() == Action::kInspect ? "正在读取诊断状态…" : "正在保存重置请求…");
        line("请等待处理结果。");
    } else if (model.page() == Page::kRecorded) {
        line("重置请求已保存。", wqn::UiTextStyle::kWarning);
        line("下次启动将清除本地数据。");
        line("短按确认重启并执行。");
    } else if (model.page() == Page::kUncertain) {
        line("重置请求可能已保存。", wqn::UiTextStyle::kWarning);
        line("写入返回错误，无法确认保存结果。", wqn::UiTextStyle::kWrappedBody);
        line("下次启动可能执行已确认的数据清除。", wqn::UiTextStyle::kWrappedBody);
        line("短按确认重启并检查。");
    } else {
        std::snprintf(text, sizeof(text), "操作未完成：%s", esp_err_to_name(model.error())); line(text);
        line("请返回重新读取诊断，或联系维护人员。", wqn::UiTextStyle::kWrappedBody);
        line("短按确认返回。");
    }
}

void SubmitAction(Model* model, Action action, TaskHandle_t ui)
{
    if (action == Action::kNone) return;
    if (action == Action::kRestart) { esp_restart(); return; }
    if (model->preservation_only() && action != Action::kExportRaw && action != Action::kCancelExport) return;
    if (action == Action::kCancelExport) {
        (void)wqn::services::CancelStorageRecoveryExport(model->operation_id()); return;
    }
    const esp_err_t error = wqn::services::StartStorageRecoveryService(ui);
    if (error != ESP_OK) { model->SubmissionFailed(error); return; }
    uint32_t id = 0;
    bool accepted = false;
    if (action == Action::kInspect) accepted = wqn::services::SubmitStorageRecoveryInspect(&id);
    else if (action == Action::kAuthorizeReset) accepted = wqn::services::SubmitStorageRecoveryReset(model->snapshot(), &id);
    else if (action == Action::kExportLogical || action == Action::kExportRaw)
        accepted = wqn::services::SubmitStorageRecoveryExport(action == Action::kExportRaw ?
            wqn::services::StorageRecoveryExportKind::kRaw : wqn::services::StorageRecoveryExportKind::kLogical, true, &id);
    if (accepted) model->Begin(action, id);
    else model->SubmissionFailed(ESP_ERR_INVALID_STATE);
}
}  // namespace

esp_err_t RunStorageRecoveryUi(esp_err_t boot_error, bool preservation_only)
{
    const TaskHandle_t ui = xTaskGetCurrentTaskHandle();
    ESP_RETURN_ON_ERROR(wqn::InitButtonInput(), kTag, "initialize recovery buttons");
    ESP_RETURN_ON_ERROR(wqn::StartButtonInputTask(ui), kTag, "start recovery buttons");
    Model model(boot_error, preservation_only);
    if (!preservation_only) SubmitAction(&model, Action::kInspect, ui);
    // Keep the large UiFrame off the IDF main stack. Recovery is the only boot
    // UI consumer and never coexists with DeviceUiTask; use one static frame.
    static wqn::UiFrame frame;
    wqn::display::DisplayRevision next_revision = 1, pending_revision = 0;
    uint64_t submitted_version = 0;
    bool dirty = true;
    while (true) {
        wqn::services::StorageRecoveryResult storage_result;
        if (wqn::services::ReadStorageRecoveryResult(&storage_result)) {
            bool applied = false;
            if (storage_result.operation == wqn::services::StorageRecoveryOperation::kInspect)
                applied = model.Inspected(storage_result.operation_id, storage_result.snapshot, storage_result.error);
            else if (storage_result.operation == wqn::services::StorageRecoveryOperation::kAuthorizeReset)
                applied = model.ResetFinished(storage_result.operation_id, storage_result.reset);
            else applied = model.ExportFinished(storage_result.operation_id, storage_result.exported, storage_result.error);
            if (applied) wqn::services::AcknowledgeStorageRecoveryResult(storage_result.operation_id);
        }
        wqn::display::DisplayResult displayed;
        while (xQueueReceive(g_display_result_queue, &displayed, 0) == pdTRUE) {
            if (displayed.revision == pending_revision) {
                pending_revision = 0;
                if (displayed.status == wqn::display::DisplayStatus::kPresented &&
                    displayed.presented_revision == displayed.revision) {
                    model.Presented(submitted_version, esp_timer_get_time() / 1000);
                } else dirty = true;
            }
            // Receive before ACK; never reclaim a revision by timeout.
            AcknowledgeDisplayResult(displayed.revision);
        }
        wqn::ButtonEvent event;
        while (wqn::ReceiveButtonEvent(&event)) {
            if (event.type == wqn::ButtonEventType::kPress && !event.repeat)
                RequestDisplayRecovery(event.occurred_at_ms);
            SubmitAction(&model, model.Input(event, esp_timer_get_time() / 1000), ui);
        }
        uint32_t progress = 0;
        if (wqn::services::ReadStorageRecoveryExportProgress(model.operation_id(), &progress)) model.ExportProgress(progress);
        if (model.version() != submitted_version) dirty = true;
        const int64_t now = esp_timer_get_time() / 1000;
        const auto recovery = CopyDisplayRecoveryStatus();
        TickType_t delay = model.exporting() ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
        if (dirty && pending_revision == 0 && recovery.CanQueue(now)) {
            BuildRecoveryFrame(model, boot_error, &frame);
            const auto submission = RequestEpdUiRefresh(frame, FrameSignature(frame), next_revision,
                RefreshSchedule::kCommit, wqn::display::WaveformRequirement::kFull);
            if (submission.accepted) {
                pending_revision = next_revision++;
                submitted_version = model.version();
                dirty = false;
            } else delay = pdMS_TO_TICKS(100);
        }
        if (pending_revision != 0) delay = pdMS_TO_TICKS(50);
        else if (dirty && recovery.HasPendingRetry()) {
            const int64_t remaining = recovery.retry_at_ms > now ? recovery.retry_at_ms - now : 1;
            const TickType_t retry = pdMS_TO_TICKS(remaining > 15000 ? 15000 : remaining);
            delay = retry == 0 ? 1 : retry;
        }
        ulTaskNotifyTake(pdTRUE, delay);
    }
}
}  // namespace device_ui_internal
