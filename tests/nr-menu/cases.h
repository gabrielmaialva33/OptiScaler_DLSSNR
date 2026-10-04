int main() {
 Config c;
 auto run = [&](bool vulkan = false) { DlssNr::RenderPassControls(&c, vulkan); assert(ImGui::stack.empty()); assert(ImGui::actions.empty()); };
 ImGui::Reset(); run(true); assert(!ImGui::Saw("Requested passes###nrPassCount")); assert(c.writes==0);
 ImGui::Reset(); c.DlssNrUseProxy.value=true; run(); assert(!ImGui::Saw("Individual pass settings###nrIndividualPassSettings")); assert(c.writes==0);
 ImGui::Reset(); c.DlssNrUseProxy.value=false; run(); assert(ImGui::Saw("Requested passes###nrPassCount")); assert(!ImGui::Saw("Edit pass###nrEditPass")); assert(c.writes==0);
 ImGui::Reset(); ImGui::actions["Individual pass settings###nrIndividualPassSettings"]=true; run(); assert(c.snapshot.Individual); assert(!ImGui::Saw("1/Inherit all master settings###nrClearPass")); assert(c.overrides.empty());
 ImGui::Reset(); ImGui::actions["1/Intensity/Override###override"]=true; run(); assert(c.overrides.at(0).Intensity==1.0f); assert(ImGui::Saw("1/Intensity/Intensity"));
 ImGui::Reset(); ImGui::actions["1/Intensity/Intensity"]=0.5; run(); assert(c.overrides.at(0).Intensity==0.5f);
 ImGui::Reset(); ImGui::actions["1/Intensity/Override###override"]=false; run(); assert(c.overrides.empty()); assert(!ImGui::Saw("1/Intensity/Intensity"));
 c.master.Intensity=1.7f;
 ImGui::Reset(); ImGui::actions["1/Intensity/Override###override"]=true; run(); assert(c.overrides.at(0).Intensity==1.7f);
 ImGui::Reset(); ImGui::actions["1/Inherit all master settings###nrClearPass"]=true; run(); assert(c.overrides.empty());
 ImGui::Reset(); ImGui::actions["Requested passes###nrPassCount"]=3; ImGui::actions["Edit pass###nrEditPass"]=3; run(); assert(c.snapshot.Count==3);
 ImGui::Reset(); ImGui::actions["3/Local structure/Override###override"]=true; ImGui::actions["3/Local tone/Override###override"]=true; ImGui::actions["3/Skin structure/Override###override"]=true; ImGui::actions["3/Model preset/Override###override"]=true; ImGui::actions["3/Model preset/Model preset"]=2; ImGui::actions["3/Style/Override###override"]=true; ImGui::actions["3/Style/Style"]=1; ImGui::actions["3/autoMask/Override auto skin mask###nrMaskOverride"]=true; ImGui::actions["3/autoMask/Auto skin mask###nrPassMask"]=false; run();
 auto third=c.overrides.at(2); assert(third.LocalStructure==1.0f && third.LocalTone==1.0f && third.SkinStructure==-1.0f && third.Preset==2 && third.Style==1 && third.AutoMask==false);
 ImGui::Reset(); ImGui::actions["Requested passes###nrPassCount"]=1; run(); assert(!ImGui::Saw("Edit pass###nrEditPass")); assert(c.overrides.at(2)==third);
 ImGui::Reset(); ImGui::actions["Individual pass settings###nrIndividualPassSettings"]=false; run(); assert(c.overrides.at(2)==third); assert(!ImGui::Saw("1/Intensity/Override###override"));
 ImGui::Reset(); ImGui::actions["Requested passes###nrPassCount"]=3; ImGui::actions["Individual pass settings###nrIndividualPassSettings"]=true; ImGui::actions["Edit pass###nrEditPass"]=3; run(); assert(c.overrides.at(2)==third);
 ImGui::Reset(); ImGui::actions["3/Inherit all master settings###nrClearPass"]=true; run(); assert(c.overrides.empty());
 // One weight set: the per-pass preset override is not drawn at all (DEVELOPMENT.md rule 2).
 DlssNr::ModelLog::configCount=1;
 ImGui::Reset(); run(); assert(!ImGui::Saw("3/Model preset/Override###override")); assert(ImGui::Saw("3/Style/Override###override"));
 DlssNr::ModelLog::configCount=-1;
 ImGui::Reset(); run(); assert(ImGui::Saw("3/Model preset/Override###override"));
 // Auto mask off: skin is drawn but inert, so a click on its override writes nothing.
 c.master.AutoMask=false;
 ImGui::Reset(); ImGui::actions["3/Skin structure/Override###override"]=true; run();
 assert(ImGui::shownDisabled.count("3/Skin structure/Override###override")); assert(c.overrides.empty());
 c.master.AutoMask=true;
 ImGui::Reset(); ImGui::actions["3/Skin structure/Override###override"]=true; run(); assert(c.overrides.at(2).SkinStructure==-1.0f);
 // Model cadence (design/model-cadence.md, DEVELOPMENT.md rule 2): nothing on native Vulkan; with the driver
 // proxy or without the shader, no control and one line only when a cadence is configured; the frame-generation
 // opt-in and the status line only above a cadence of 1; a hand-edited out-of-range value reads as off.
 const std::string combo = "Model cadence###nrCadence", optIn = "Allow with frame generation###nrCadenceFrameGen";
 auto cadence = [&](bool vulkan = false) { DlssNr::RenderCadenceControls(&c, vulkan); assert(ImGui::stack.empty()); assert(ImGui::actions.empty()); };
 auto said = [](const char* text) { return std::any_of(ImGui::texts.begin(), ImGui::texts.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; }); };
 c.DlssNrCadence.value=2;
 ImGui::Reset(); cadence(true); assert(!ImGui::Saw(combo) && !ImGui::Saw(optIn) && ImGui::texts.empty());
 c.DlssNrUseProxy.value=true; c.DlssNrCadence.value=1;
 ImGui::Reset(); cadence(); assert(!ImGui::Saw(combo) && ImGui::texts.empty());
 c.DlssNrCadence.value=2;
 ImGui::Reset(); cadence(); assert(!ImGui::Saw(combo) && !ImGui::Saw(optIn) && ImGui::texts.size()==1 && said("driver proxy"));
 c.DlssNrUseProxy.value=false; DlssNr::cadenceAvailable=false;
 ImGui::Reset(); cadence(); assert(!ImGui::Saw(combo) && ImGui::texts.size()==1 && said("no model cadence shader"));
 DlssNr::cadenceAvailable=true; c.DlssNrCadence.value=1;
 ImGui::Reset(); cadence(); assert(ImGui::Saw(combo) && !ImGui::Saw(optIn));
 ImGui::Reset(); ImGui::actions[combo]=1; cadence(); assert(c.DlssNrCadence.value==2);
 ImGui::Reset(); cadence(); assert(ImGui::Saw(combo) && ImGui::Saw(optIn) && !c.DlssNrCadenceWithFrameGen.value);
 ImGui::Reset(); ImGui::actions[optIn]=true; cadence(); assert(c.DlssNrCadenceWithFrameGen.value);
 ImGui::Reset(); ImGui::actions[combo]=3; cadence(); assert(c.DlssNrCadence.value==4);
 ImGui::Reset(); ImGui::actions[combo]=0; cadence(); assert(c.DlssNrCadence.value==1);
 ImGui::Reset(); cadence(); assert(ImGui::Saw(combo) && !ImGui::Saw(optIn));
 c.DlssNrCadence.value=9;
 ImGui::Reset(); cadence(); assert(ImGui::Saw(combo) && !ImGui::Saw(optIn));
 std::cout << "PASS: 31 scripted menu frames: unsupported paths, inheritance, seven fields, clear, retained inactive passes, preset hidden at one weight set, skin inert with the mask off; model cadence hidden on native Vulkan, a line only under the proxy or without its shader, the frame-generation opt-in only above a cadence of 1\n";
}
