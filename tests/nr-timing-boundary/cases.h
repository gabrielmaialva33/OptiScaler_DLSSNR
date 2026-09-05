int main() {
    int cases=0;
    Config cfg;
    DlssNrPassSnapshot passes;
    auto make=[&](const char* stage="after", unsigned w=3440, unsigned h=1440, float scale=1.0f,
                  bool reset=false, bool capture=false, bool hdr=true, unsigned format=10) {
        return TimingMetadata(cfg,passes,42,17,stage,2293,960,w,h,unsigned(w*scale),unsigned(h*scale),scale,reset,capture,hdr,format);
    };
    auto a=make(), b=make();
    assert(a.settingsGeneration==b.settingsGeneration && a.contractHash==b.contractHash); ++cases;
    auto before=make("before",2293,960);
    assert(before.settingsGeneration>a.settingsGeneration && before.contractHash!=a.contractHash);
    assert(before.outputWidth==2293 && before.renderWidth==2293 && before.modelWidth==2293); ++cases;
    auto scaled=make("after",3440,1440,2.0f);
    assert(scaled.modelWidth==6880 && scaled.workingScale==2 && scaled.contractHash!=a.contractHash); ++cases;
    auto normal=make(), reset=make("after",3440,1440,1,true,true);
    assert(reset.settingsGeneration==normal.settingsGeneration && reset.modelReset && reset.captureActive);
    assert(reset.evaluationId==42 && reset.presentId==17); ++cases;
    passes.Count=2; passes.Individual=true;
    auto two=make(); passes.Settings[1].Intensity=0.7f; auto edited=make();
    assert(two.contractHash!=edited.contractHash && edited.requestedPasses==2); ++cases;
    cfg.DlssNrApplyModel.value=false; auto hidden=make();
    assert(hidden.contractHash!=edited.contractHash && std::string(hidden.modelIdentity)=="fixture-file-attributes"); ++cases;
    auto sdr=make("after",3440,1440,1,false,false,false,28);
    assert(sdr.contractHash!=hidden.contractHash); ++cases;
    assert(a.modelWidth==3440 && a.settingsGeneration<hidden.settingsGeneration); ++cases;

    using DlssNr::RenderGpuTiming;
    ImGui::Clear(); RenderGpuTiming(&cfg,true);
    assert(ImGui::checkboxCalls==0 && ImGui::sliderCalls==0 && ImGui::Contains("not instrumented")); ++cases;
    cfg.DlssNrUseProxy.value=true; ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(ImGui::checkboxCalls==0); ++cases;
    cfg.DlssNrUseProxy.value=false; ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(ImGui::checkboxCalls==1 && ImGui::sliderCalls==0 && snapshotReads==0 && ImGui::Contains("Measurement off")); ++cases;
    ImGui::toggle=1; ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(cfg.settings.Enabled && cfg.writes==1 && ImGui::sliderCalls==1 && ImGui::Contains("No confirmed")); ++cases;
    ImGui::interval=17; ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(cfg.settings.Interval==17 && cfg.writes==2 && ImGui::Contains("17 evaluations")); ++cases;
    published.valid=true; published.sampleId=12; published.sampleAgeMs=2500; published.metadata=a;
    published.sampleInterval=30; published.interval=17; published.actualPasses=1;
    published.totalMs=6; published.modelMs=4; published.otherMs=2;
    ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(ImGui::Contains("Historical sample 12, 2.5 s") && ImGui::Contains("interval was 30 evaluations"));
    assert(ImGui::Contains("not the current frame") && !ImGui::Contains("ms per frame")); ++cases;
    ImGui::toggle=0; ImGui::Clear(); RenderGpuTiming(&cfg,false);
    assert(!cfg.settings.Enabled && ImGui::sliderCalls==0 && !ImGui::Contains("Last confirmed")); ++cases;
    assert(cases==15);
    std::printf("PASS %d timing boundary metadata/UI cases\n",cases);
}
