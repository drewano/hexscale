import {
  definePlugin,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  SliderField,
  DropdownItem,
  ServerAPI,
  staticClasses
} from "decky-frontend-lib";
import { VFC, useState, useEffect } from "react";
import { FaMicrochip } from "react-icons/fa";

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

const HexscalePanel: VFC<{ serverApi: ServerAPI }> = ({ serverApi }) => {
  const [enabled, setEnabled] = useState<boolean>(true);
  const [sharpness, setSharpness] = useState<number>(75);
  const [profile, setProfile] = useState<number>(1);
  const [status, setStatus] = useState<StatusData | null>(null);
  const [online, setOnline] = useState<boolean>(false);

  const fetchStatus = async () => {
    try {
      const resp = await serverApi.callPluginMethod<Record<string, unknown>, { success: boolean; data?: StatusData }>("get_status", {});
      if (resp.success && resp.result?.success && resp.result.data) {
        setStatus(resp.result.data);
        setEnabled(resp.result.data.enabled);
        setSharpness(Math.round(resp.result.data.sharpness * 100));
        setProfile(resp.result.data.profile);
        setOnline(true);
      } else {
        setOnline(false);
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
    setEnabled(newVal);
    await serverApi.callPluginMethod("set_enabled", { enabled: newVal });
  };

  const handleSharpnessChange = async (newVal: number) => {
    setSharpness(newVal);
    await serverApi.callPluginMethod("set_sharpness", { sharpness: newVal / 100.0 });
  };

  const handleProfileChange = async (newProfile: number) => {
    setProfile(newProfile);
    await serverApi.callPluginMethod("set_profile", { profile: newProfile });
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
          disabled={!online}
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
              disabled={!online}
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
              disabled={!online}
              onChange={(opt) => handleProfileChange(opt.data)}
            />
          </PanelSectionRow>
        </>
      )}

      <PanelSection title="NPU Telemetry">
        <PanelSectionRow>
          <div style={{ display: "flex", flexDirection: "column", gap: "6px", fontSize: "12px" }}>
            <div style={{ display: "flex", justifyContent: "space-between" }}>
              <span style={{ color: "#8a8a8a" }}>Daemon Status:</span>
              <span style={{ color: online ? "#66c0f4" : "#e03b3b", fontWeight: "bold" }}>
                {online ? "ACTIVE (Online)" : "OFFLINE"}
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
                  <span style={{ color: "#66c0f4", fontWeight: "bold" }}>{status.last_inference_ms} ms</span>
                </div>
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ color: "#8a8a8a" }}>Average Latency:</span>
                  <span>{status.avg_inference_ms} ms</span>
                </div>
              </>
            )}
          </div>
        </PanelSectionRow>
      </PanelSection>
    </PanelSection>
  );
};

export default definePlugin((serverApi: ServerAPI) => {
  return {
    title: <div className={staticClasses.Title}>Hexscale</div>,
    content: <HexscalePanel serverApi={serverApi} />,
    icon: <FaMicrochip />,
    onDismount() {}
  };
});
