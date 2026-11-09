
int main()
{
    using namespace device_ui_internal;
    int total=0,failed=0;
    auto check=[&](bool ok,const char* label) {
        ++total;
        if(!ok) { ++failed;std::fprintf(stderr,"FAIL STD handoff: %s\n",label); }
    };
    const auto types={wqn::ButtonEventType::kPress,wqn::ButtonEventType::kHoldPress,
        wqn::ButtonEventType::kShortPress,wqn::ButtonEventType::kDoublePress,
        wqn::ButtonEventType::kLongPress,wqn::ButtonEventType::kLongRelease,
        wqn::ButtonEventType::kRelease};
    for(int selected:{0,4}) for(auto button:{wqn::ButtonId::kConfirm,wqn::ButtonId::kUp,wqn::ButtonId::kDownPower})
        for(auto type:types) {
            wqn::UiState state;state.ai.tier=wqn::AiTier::kStd;
            state.ai.status=wqn::AiSessionStatus::kTranscribing;
            state.status_edit.active=true;state.status_edit.selected=selected;
            state.gestures.std_decision_press_ms=1000;state.gestures.last_ai_confirm_tap_ms=1000;
            const int calls=std_status_bar_calls;
            RouteStdOrStatusBar({button,type},2000,&state);
            check(std_status_bar_calls==calls,"transcribing gesture cannot reach mode/trash handler");
            check(!state.status_edit.active,"transcribing gesture closes an old editor");
            check(state.ai.tier==wqn::AiTier::kStd&&state.ai.status==wqn::AiSessionStatus::kTranscribing,
                  "transcribing gesture retains the current operation");
            check(state.gestures.std_decision_press_ms==0&&state.gestures.last_ai_confirm_tap_ms==0,
                  "transcribing consumes the entire old gesture");
        }
    UiRuntime runtime;runtime.state_.ai.tier=wqn::AiTier::kStd;
    runtime.state_.status_edit.active=true;runtime.state_.gestures.last_ai_confirm_tap_ms=1200;
    wqn::AiSessionState snapshot; snapshot.tier=wqn::AiTier::kStd;
    snapshot.status=wqn::AiSessionStatus::kTranscribing;
    runtime.DispatchAiSessionSnapshot(snapshot);
    check(!runtime.state_.status_edit.active&&runtime.state_.gestures.last_ai_confirm_tap_ms==0,
          "transcribing snapshot closes the editor before any button arrives");
    auto& state=runtime.state_;
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},2100,&state);
    snapshot.status=wqn::AiSessionStatus::kAwaitingConfirmation;
    snapshot.pending_input="synthetic unsent input";snapshot.input_armed_at_ms=2500;
    runtime.DispatchAiSessionSnapshot(snapshot);wqn::std_backend=state.ai;
    const int sends=wqn::std_sends;
    for(auto type:{wqn::ButtonEventType::kRelease,wqn::ButtonEventType::kShortPress,wqn::ButtonEventType::kDoublePress})
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,type},2600,&state);
    check(wqn::std_sends==sends,"PTT release tails cannot send a newly armed transcript");
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},2700,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},2800,&state);
    check(wqn::std_sends==sends+1,"a fresh confirmation sends exactly once");
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kDoublePress},2900,&state);
    check(wqn::std_sends==sends+1,"a duplicate gesture never resends the confirmed transcript");
    for(auto tier:{wqn::AiTier::kStd,wqn::AiTier::kAgent,wqn::AiTier::kFlash}) {
        state={};state.ai.tier=tier;state.ai.status=wqn::AiSessionStatus::kIdle;state.status_edit.active=true;
        const int calls=std_status_bar_calls;
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},3000,&state);
        check(std_status_bar_calls==calls+1,"idle status editor remains available");
    }
    // Producer emits HoldPress at 200 ms and still emits ShortPress on release
    // before 650 ms. Its raw Release is deferred to the following poll.
    for(int duration:{200,400,649}) {
        state={};state.ai.tier=wqn::AiTier::kStd;
        state.ai.status=wqn::AiSessionStatus::kAwaitingConfirmation;
        state.ai.pending_input="synthetic unsent input";state.ai.input_armed_at_ms=5000;
        wqn::std_backend=state.ai;const int before=wqn::std_sends;
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},6000,&state);
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kHoldPress},6200,&state);
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},6000+duration,&state);
        RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kRelease},6010+duration,&state);
        check(wqn::std_sends==before+1,"producer short press after 200ms hold must still confirm");
    }
    state={};state.ai.tier=wqn::AiTier::kStd;state.ai.status=wqn::AiSessionStatus::kTranscribing;
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},6000,&state);
    snapshot=state.ai;snapshot.status=wqn::AiSessionStatus::kAwaitingConfirmation;
    snapshot.pending_input="synthetic unsent input";snapshot.input_armed_at_ms=6250;
    state.ai=snapshot;wqn::std_backend=snapshot;const int tail_sends=wqn::std_sends;
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kHoldPress},6300,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},6400,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kRelease},6410,&state);
    check(wqn::std_sends==tail_sends,"a held PTT tail still has no fresh confirmation press");
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},7000,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kHoldPress},7200,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kLongPress},7650,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kLongRelease},7700,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kRelease},7710,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},7720,&state);
    check(wqn::std_sends==tail_sends,"a long decision gesture and its tail cannot submit");
    state={};state.ai.tier=wqn::AiTier::kStd;state.ai.status=wqn::AiSessionStatus::kAwaitingDecision;
    state.ai.interaction.id="synthetic-question";state.ai.interaction.armed_at_ms=5000;
    state.ai.interaction.options={{"yes","Yes"}};wqn::std_backend=state.ai;const int answers=wqn::std_answers;
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kPress},6000,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kHoldPress},6200,&state);
    RouteStdOrStatusBar({wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress},6400,&state);
    check(wqn::std_answers==answers+1&&state.ai.interaction.sending,"mid-length short press can answer a decision once");

    std::printf("STD handoff: %d checks, %d failures (production UI dispatch/snapshot; host ports; NOT HIL)\n",total,failed);
    return failed?1:0;
}
