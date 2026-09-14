import {
  definePlugin,
  callable
} from "@decky/api";
import {
  PanelSection,
  PanelSectionRow,
  ToggleField,
  SliderField,
  DropdownItem,
  staticClasses
} from "@decky/ui";
import React, { VFC, useState, useEffect } from "react";

interface StatusData {
  enabled: boolean;
  profile: number;
  sharpness: number;
  last_inference_ms: number;
  avg_inference_ms: number;
  total_frames: number;
  model_name: string;
  soc: string;
  backend: string;
}

interface StatusResponse {
  success: boolean;
  online?: boolean;
  data?: StatusData;
  error?: string;
}

const getStatus = callable<[], StatusResponse>("get_status");
const setEnabled = callable<[boolean], { success: boolean }>("set_enabled");
const setSharpness = callable<[number], { success: boolean }>("set_sharpness");
const setProfile = callable<[number], { success: boolean }>("set_profile");

const HexscaleIcon: VFC = () => (
  <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor">
    <path d="M4 4h4v4H4V4zm6 0h4v4h-4V4zm6 0h4v4h-4V4zM4 10h4v4H4v-4zm6 0h4v4h-4v-4zm6 0h4v4h-4v-4zM4 16h4v4H4v-4zm6 0h4v4h-4v-4zm6 0h4v4h-4v-4z"/>
  </svg>
);

const HexscalePanel: VFC = () => {
  const [enabled, setEnabledState] = useState<boolean>(true);
  const [sharpness, setSharpnessState] = useState<number>(75);
  const [profile, setProfileState] = useState<number>(1);
  const [status, setStatus] = useState<StatusData | null>(null);
  const [online, setOnline] = useState<boolean>(false);

  const fetchStatus = async () => {
    try {
      const resp = await getStatus();
      if (resp && resp.success && resp.data) {
        setStatus(resp.data);
        setEnabledState(resp.data.enabled);
        setSharpnessState(Math.round(resp.data.sharpness * 100));
        setProfileState(resp.data.profile);
        setOnline(!!resp.online);
      }
    } catch {
      setOnline(false);
    }
  };

  useEffect(() => {
    fetchStatus();
    const interval = setInterval(fetchStatus, 2000);
    return () => clearInterval(interval);
  }, []);

  const handleToggle = async (newVal: boolean) => {
    setEnabledState(newVal);
    await setEnabled(newVal);
  };

  const handleSharpnessChange = async (newVal: number) => {
    setSharpnessState(newVal);
    await setSharpness(newVal / 100.0);
  };

  const handleProfileChange = async (newProfile: number) => {
    setProfileState(newProfile);
    await setProfile(newProfile);
  };

  const profileOptions = [
    { data: 0, label: "Efficiency (Low Power)" },
    { data: 1, label: "Balanced (Adaptive HTP)" },
    { data: 2, label: "Burst (Sub-ms Priority)" }
  ];

  return (
    <PanelSection title="Hexscale NPU Super-Resolution">
      <PanelSectionRow>
        <ToggleField
          label="Enable NPU Upscaling"
          description="Offload spatial upscaling to Qualcomm Hexagon CDSP"
          checked={enabled}
          onChange={handleToggle}
        />
      </PanelSectionRow>

      {enabled && (
        <>
          <PanelSectionRow>
            <SliderField
              label="Texture Sharpness"
              value={sharpness}
              min={0}
              max={100}
              step={5}
              showValue={true}
              valueSuffix="%"
              onChange={handleSharpnessChange}
            />
          </PanelSectionRow>

          <PanelSectionRow>
            <DropdownItem
              label="NPU Clock Profile"
              description="Adjust Hexagon HTP performance vs power envelope"
              menuLabel="Select Profile"
              rgOptions={profileOptions}
              selectedOption={profile}
              onChange={(opt) => handleProfileChange(opt.data)}
            />
          </PanelSectionRow>
        </>
      )}

      <PanelSection title="NPU Telemetry">
        <PanelSectionRow>
          <div style={{ display: "flex", flexDirection: "column", gap: "6px", fontSize: "12px" }}>
            <div style={{ display: "flex", justifyContent: "space-between" }}>
              <span style={{ color: "#8a8a8a" }}>NPU Subsystem:</span>
              <span style={{ color: online ? "#a3e635" : "#eab308", fontWeight: "bold" }}>
                {online ? "ACTIVE (Inference)" : "STANDBY (Driver Ready)"}
              </span>
            </div>
            <div style={{ display: "flex", justifyContent: "space-between" }}>
              <span style={{ color: "#8a8a8a" }}>Daemon Status:</span>
              <span style={{ color: online ? "#66c0f4" : "#94a3b8" }}>
                {online ? "CONNECTED" : "AWAITING GAME"}
              </span>
            </div>
            {status && (
              <>
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ color: "#8a8a8a" }}>Hardware Target:</span>
                  <span>{status.soc} ({status.backend})</span>
                </div>
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ color: "#8a8a8a" }}>Active Model:</span>
                  <span>{status.model_name}</span>
                </div>
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ color: "#8a8a8a" }}>Last Inference Time:</span>
                  <span style={{ color: online ? "#66c0f4" : "#8a8a8a", fontWeight: "bold" }}>
                    {online ? `${status.last_inference_ms} ms` : "0.0 ms"}
                  </span>
                </div>
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ color: "#8a8a8a" }}>Average Latency:</span>
                  <span>{online ? `${status.avg_inference_ms} ms` : "0.0 ms"}</span>
                </div>
              </>
            )}
          </div>
        </PanelSectionRow>
      </PanelSection>
    </PanelSection>
  );
};

export default definePlugin(() => {
  return {
    name: "Hexscale",
    titleView: <div className={staticClasses.Title}>Hexscale</div>,
    content: <HexscalePanel />,
    icon: <HexscaleIcon />,
    alwaysRender: true,
    onDismount() {}
  };
});
