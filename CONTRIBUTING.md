# Contributing

## Scope

Contributions should make a migration safer, clearer, or easier to verify. Changes that broaden the migration scope must explain why the data is player-owned and why copying it will not overwrite the destination modpack baseline.

## Reporting an issue

Include the following when reporting a bug:

- Sempervirens version
- Minecraft version and loader
- The selected migration profile
- The expected and actual result
- Relevant migration report text, with personal paths or server addresses removed when needed

Do not attach private worlds, account data, or full instance archives unless a maintainer explicitly requests them through an approved private channel.

## Making a change

1. Keep the change focused on one outcome.
2. Update the relevant profile, documentation, and tests together.
3. Run `Build.cmd` and confirm that the core, updater, and native UI tests pass.
4. Describe user-visible behavior and migration risks in the review request.

## Code expectations

- Preserve the source-read-only guarantee.
- Treat every profile path and filesystem entry as untrusted input.
- Prefer explicit failure messages over silently skipping an operation.
- Keep user-facing copy direct, specific, and free of implementation jargon.
