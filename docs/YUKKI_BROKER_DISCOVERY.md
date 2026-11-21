# YuKKi-OS broker service discovery contract

Overhauled remains an execution service; YuKKi-OS advertises and discovers
broker endpoints through its existing authenticated peer mesh. The current
implementation is lease-based gossip, not Kademlia DHT routing.

## Advertisement

A YuKKi-OS node advertises a co-located or reachable Overhauled broker when
`YUKKI_BROKER_ENDPOINT=host:port` is configured. Optional
`YUKKI_BROKER_CAPABILITIES` is a comma-separated descriptive list such as
`cpu,gpu,npu`. Without the endpoint variable, no broker record is emitted.

The versioned `overhauled.worker-broker` record carries a service UUID, node
UUID, endpoint, BRK1/BRK2 protocol versions, coarse capabilities, frame limit,
lease timestamps, message UUID, and gossip hop budget. YuKKi validates records,
caps leases at 300 seconds, refreshes its own record every 30 seconds, expires
stale records, caps the registry at 4,096 entries, and limits forwarding to
eight hops.

## Security

Discovery messages travel inside YuKKi-OS's PSK-protected peer channel. This
is cluster-level shared-secret authentication, not individual peer identity.
A discovery record is not authorization to execute work.

The Overhauled broker's BRK1/BRK2 listener has no built-in authentication or
TLS. A service endpoint must therefore be reachable only through a trusted
private network or an independently authenticated mTLS proxy/service mesh.
Never advertise an unprotected public listener. Keep raw CPU/NUMA topology and
tenant data out of service records.

## Execution contract

After discovery, clients negotiate/validate the supported BRK schema and
submit tasks only after their own admission checks. BRK2 response field 7 is
the authoritative execution backend; advertised `gpu` or `npu` capability
does not guarantee that hardware is present or selected. Overhauled retains
full authority over local CPU-group/NUMA placement and device dispatch.

The record schema is documented in YuKKi-OS
[`docs/BROKER_SERVICE_DISCOVERY.md`](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/BROKER_SERVICE_DISCOVERY.md).
