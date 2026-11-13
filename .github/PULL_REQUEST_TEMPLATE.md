## What and why

<!-- What does this change, and why? Link the issue if there is one: "Fixes #123". -->

## How I tested it

<!-- Commands you ran and what you checked by hand. Add a screenshot for UI changes. -->

- [ ] Debug build has no warnings (`-DCMAKE_BUILD_TYPE=Debug` builds with `-Werror`)
- [ ] `ctest --output-on-failure` passes
- [ ] New logic has a unit test in `tests/`
- [ ] Sanitizer build is clean (for changes to memory, strings or the job runner; see CONTRIBUTING.md)
