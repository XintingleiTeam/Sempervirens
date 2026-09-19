# Security Policy

## Security boundary

Sempervirens handles local Minecraft instance files. It is designed to keep the selected source instance read-only and to constrain migration targets to the selected destination instance.

Path validation rejects absolute paths, drive letters, `..` traversal, and reparse points encountered while planning a migration. The application also refuses source and destination paths that are identical or nested within one another.

## Responsible disclosure

Report a security issue through the project's private maintainer channel. Include a minimal reproduction and avoid publishing exploit details before a fix is available.

Please report issues involving:

- Writes outside the selected destination instance
- Source-instance modification
- Bypassing path or reparse-point validation
- Data loss not covered by the selected overwrite strategy
- Unsafe parsing of profile or NBT data
- Update manifest signature bypasses, mirror validation failures, or rollback failures

## Operational guidance

Close Minecraft and its launcher before migration. A running game can update files while they are being read or replaced, which can produce an incomplete or inconsistent result.
