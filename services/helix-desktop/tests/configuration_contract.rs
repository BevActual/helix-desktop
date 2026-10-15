//! Rust behavioural contract for `org.helix.Desktop` (`contract/` modules).
//!
//!   tools/nested/helix-desktop-configuration-contract.sh

mod contract;
mod harness;

use harness::contract_enabled;

#[tokio::test]
async fn desktop_service_configuration_contract() {
    if !contract_enabled() {
        eprintln!(
            "skip configuration_contract (set HELIX_RUN_CONFIGURATION_CONTRACT=1; see tools/nested/helix-desktop-configuration-contract.sh)"
        );
        return;
    }
    contract::run_all().await;
}
