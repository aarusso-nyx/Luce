# OTA and Recovery Model

## OTA Features

- Feature-gated OTA service (`ota` flag)
- URL, CA hash, channel, and auto-check period are persisted in NVS
- Trigger path is command driven (`CommandAction::OTA_START`)
- OTA task executes in own service task (`ServiceId::OTA`)

## OTA Preconditions

- HTTPS URL required in policy
- trust material (`ca_pem_hash` or equivalent chain policy) required for secure operations
- privileged command context required

## Failure Handling

- Restart exhaustion and failed critical services are represented as service faults.
- `publish_fault()` pushes high-severity fault events into event stream.
- Faults propagate to supervisor level and error indicators.

## Recovery Behavior

- Supervisor restarts failed/recoverable services using exponential backoff table from `app_config.h`.
- Exceeded restart budget transitions service into `FAILED`.
- Core services entering prolonged fail state may escalate to full reboot pathway.
