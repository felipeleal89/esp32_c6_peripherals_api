# Contributing

Thank you for contributing.

## Contribution Rules

- By submitting code, you agree the contribution is licensed under `0BSD`.
- Contributions do not imply named credit requirements.
- Do not claim endorsement, affiliation, sponsorship, or authorship on behalf of others.

## Engineering Expectations

- Keep changes small and reviewable.
- Separate mechanical formatting from logic changes.
- Avoid functional changes unless fixing a verified defect.
- Keep memory/timing characteristics close to existing behavior.
- Prefer fixed-width types and explicit error handling.
- Keep documentation and comments in English.

## Pull Request Checklist

- Build succeeds in ESP-IDF.
- Public headers remain backward compatible unless explicitly documented.
- New/changed APIs include minimal Doxygen comments.
- Documentation is in English.
- No unrelated file churn in the same commit.

## Suggested Commit Style

- `feat(<module>): short description`
- `fix(<module>): short description`
- `refactor(<module>): short description`
- `docs: short description`

Examples:

- `feat(knob_api): add polling-based button click event`
- `fix(main): avoid watchdog starvation in idle loop`
