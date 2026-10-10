{
  config,
  lib,
  ...
}:
let
  cfg = config.programs.helix;
in
{
  # The module from this flake predates the module in nixpkgs.
  # To avoid conflicts, we disable the nixpkgs module.
  disabledModules = [ "programs/wayland/helix.nix" ];

  options.programs.helix = {
    enable = lib.mkEnableOption "Whether to enable Helix Shell (helix), a niri-first Wayland desktop shell.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The helix package to install.";
    };

    systemd = {
      enable = lib.mkEnableOption "Enables a systemd user service for helix.";

      target = lib.mkOption {
        type = lib.types.str;
        default = "graphical-session.target";
        example = "hyprland-session.target";
        description = "The systemd user target for the helix service.";
      };
    };

    recommendedServices.enable = lib.mkEnableOption ''
      NixOS services used by Helix Shell integrations, including NetworkManager,
      Bluetooth, UPower, and a power profile service.
    '';
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        hardware.graphics.enable = lib.mkDefault true;

        environment.systemPackages = lib.optional (cfg.package != null) cfg.package;

        systemd.user.services.helix = lib.mkIf cfg.systemd.enable {
          description = "Helix Shell (helix)";
          documentation = [ "https://github.com/BevActual/helix-shell/tree/main/docs/user/" ];
          partOf = [ cfg.systemd.target ];
          after = [ cfg.systemd.target ];
          wantedBy = [ cfg.systemd.target ];
          restartTriggers = [ cfg.package ];

          environment.PATH = lib.mkForce null;

          serviceConfig = {
            ExecStart = lib.getExe cfg.package;
            Restart = "on-failure";
          };
        };

        assertions = [
          {
            assertion = !cfg.systemd.enable || cfg.package != null;
            message = "programs.helix.package cannot be null when programs.helix.systemd.enable is true";
          }
        ];
      }

      (lib.mkIf cfg.recommendedServices.enable {
        networking.networkmanager.enable = lib.mkDefault true;
        hardware.bluetooth.enable = lib.mkDefault true;
        services.upower.enable = lib.mkDefault true;
        services.power-profiles-daemon.enable = lib.mkIf (!config.services.tuned.enable) (
          lib.mkDefault true
        );
      })
    ]
  );
}
