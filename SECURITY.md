# Security Policy

AtlasLOB is an educational systems project and is not hardened for public Internet exposure.

## Supported versions

Only the latest tagged release receives fixes. Before the first release, use the current `main`
branch.

## Reporting

Please report vulnerabilities privately through GitHub's security-advisory feature rather than a
public issue.

## Current boundaries

The project has no network listener in its current phase. A future gateway will bind to loopback by
default, cap frames and queues before allocation, fuzz byte decoders, and document its threat model.
