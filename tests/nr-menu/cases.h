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
 std::cout << "PASS: 15 scripted menu frames: unsupported paths, inheritance, seven fields, clear, retained inactive passes\n";
}
