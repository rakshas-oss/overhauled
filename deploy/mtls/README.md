# Overhauled broker mTLS sidecar profile

These examples terminate mutually authenticated TLS 1.3 in stunnel while
keeping the existing BRK1/BRK2 byte protocol unchanged. They are a deployable
network boundary, not an in-process TLS implementation.

## Listener topology

- Overhauled listens only on loopback at 127.0.0.1:19091.
- The server-side stunnel accepts mTLS on TCP 9090 and forwards to the
  loopback broker.
- The YuKKi-side stunnel listens only on 127.0.0.1:19090 and establishes
  mTLS to the server-side stunnel.
- YuKKi-OS connects to the local sidecar endpoint, never to the remote raw
  broker listener.

Start the broker with:

```sh
broker_server 19091 --bind-address 127.0.0.1 --require-mtls-proxy
```

Configure the YuKKi-OS process with:

```sh
YUKKI_GPU_BROKER_ENDPOINT=127.0.0.1:19090
YUKKI_GPU_REQUIRE_MTLS_PROXY=true
YUKKI_GPU_MAX_CONNECTIONS=16
```

The strict proxy setting rejects a non-loopback or non-numeric socket
endpoint. It ensures the application uses a local TLS terminator; it does
not itself create or inspect the TLS session.

## Certificate requirements

Use a dedicated server CA and client CA where practical. The server certificate
must identify broker.example.net in its SAN. Each YuKKi node must receive a
distinct client certificate and private key. The server-side chain verification
requires a trusted client certificate; client-side chain verification plus
hostname checking validates the server chain and service identity. Both configs
require TLS 1.3.

Do not store private keys, CA private keys, or production certificates in the
repository. Protect the key files with restrictive filesystem permissions.
Use an overlapping trust bundle during planned certificate rotation; validate
the new certificate before removing the old trust anchor. Firewall the raw
broker port 19091 so only loopback can reach it, and do not expose the
client-side plaintext port 19090 to any non-loopback interface.

Validate the installed stunnel version and configuration syntax before
deployment:

```sh
stunnel -version
stunnel -test deploy/mtls/server.conf
stunnel -test deploy/mtls/client.conf
```

The exact binary/package command may vary by distribution.

## Security boundary and remaining integration

The mTLS proxy authenticates the network peer and protects BRK1/BRK2 traffic
in transit. The existing handoff API independently verifies signed deployment
messages and publisher provenance using its configured Ed25519 trust stores.
The broker does not yet receive the peer certificate fingerprint from stunnel
or bind that TLS identity to the signed handoff sender identity. Do not treat
those two identities as already joined. A future native TLS listener/transport
adapter must pass a verified peer identity into handoff admission and compare
it against the signed sender binding before accepting a deployment.

The broker's nonce journal can persist replay decisions when a journal path is
configured, but prepared artifacts, active-release state and runtime state
remain process-local. This profile does not provide restart-safe artifact
staging or a distributed activation transaction.
