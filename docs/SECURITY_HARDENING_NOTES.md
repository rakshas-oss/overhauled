# Security hardening notes

This note summarizes the hardening relevant to YuKKi-OS/Overhauled interoperability. The wire protocols remain byte-compatible: changes tighten validation and resource bounds without changing BRK1/BRK2 envelope layout or field numbering.

- BRK2 clients now reject duplicate singular fields as they are decoded and validate each field value before retaining it. Explicitly repeated fields continue to be accepted only when every occurrence carries the repeated flag.
- The browser client preserves exact unsigned protocol integers in the `latest_seq`, `sequence_no`, and `timestamp_ms` JSON fields, avoiding binary64 rounding during parse.
- Dashboard bearer credentials are held only in page memory, sent with `cache: no-store`, cleared on disconnect/page exit, and never written to local storage.
- Native mesh and guest-worker safety updates are described in [SECURITY_HARDENING_NOTES.md](SECURITY_HARDENING_NOTES.md) in this repository.

These changes do not authenticate the broker wire protocol or replace TLS. Use authenticated transport and protect credentials in deployment.
