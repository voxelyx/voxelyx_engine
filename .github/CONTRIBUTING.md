# Contributing

Contributions are welcome! Here's how you can help:

- [Contributing code](#code)
- [Reporting issues](#issues)
- [Requesting features](#feature-requests)

## Code

1. [Fork](https://help.github.com/articles/fork-a-repo/) the repository and
   [clone](https://help.github.com/articles/cloning-a-repository/) your fork.

2. Before you start coding, consider opening an
   [issue on GitHub](https://github.com/voxelyx/voxelyx_engine/issues) to discuss the
   suitability and implementation of your intended contribution.

3. Start coding!
    - Refer to the
      [Lua API](https://github.com/voxelyx/voxelyx_engine/blob/main/doc/lua_api.md) and other
      [documentation](https://github.com/voxelyx/voxelyx_engine/tree/main/doc).
    - Follow the [AI policy](https://github.com/voxelyx/voxelyx_engine/blob/main/doc/developing/ai_policy.md).
    - Check your code works as expected and document any changes to the Lua API.
    - To avoid conflicting changes between contributions, do not do the following manually. They will be done before each release.
      - Run `updatepo.sh` or update `voxelyx.po{,t}` even if your code adds new translatable strings.
      - Update `main.conf.example` and `settings_translation_file.cpp` even if your code adds new core settings.

4. Commit & [push](https://help.github.com/articles/pushing-to-a-remote/) your changes to a new branch (one change per branch)
    - Commit messages should:
        - Use the present tense.
        - Be descriptive.
    - The first line should:
        - Start with a capital letter.
        - Be a compact summary of the commit.
        - Preferably have less than 70 characters.
        - Have no full stop at the end.
    - The second line should be empty.
    - The following lines should describe the commit, starting a new line for each point.

5. Once you are happy with your changes, submit a pull request.
     - Open the [pull-request form](https://github.com/voxelyx/voxelyx_engine/pull/new/main).
     - Add a description explaining what you've done (or if it's a
       work-in-progress - what you need to do).
     - Make sure to fill out the pull request template.

### A pull-request is considered merge-able when:

1. It works.
2. It follows [the AI policy](https://github.com/voxelyx/voxelyx_engine/blob/main/doc/developing/ai_policy.md)
   for all code, documentation, and other media.
3. The code's interfaces are well designed, regardless of other aspects that
   might need more work in the future.

## Issues

If you experience an issue, we would like to know the details.

1. Do a quick search on GitHub to check if the issue has already been reported.
2. Is it an issue with the Voxelyx *engine*? If it is about a particular game,
   report it to the developers of that game instead.
3. [Open an issue](https://github.com/voxelyx/voxelyx_engine/issues/new) and describe
   the issue you are having - you could include:
     - Error logs (check the bottom of the `debug.txt` file).
     - Screenshots.
     - Ways you have tried to solve the issue, and whether they worked or not.
     - Your Voxelyx version and the content (games, mods or texture packs) you have installed.
     - Your platform (e.g. Windows 10 or Ubuntu 15.04 x64).

After reporting you should aim to answer questions or clarifications as this
helps pinpoint the cause of the issue.

## Feature requests

Feature requests are welcome. You should provide a clear explanation with as
much detail as possible.
