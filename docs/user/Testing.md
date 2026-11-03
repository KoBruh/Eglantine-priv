# Testing

When you're testing a pull request (PR) and encounter unexpected behavior, it's important to determine whether the issue was introduced by the PR or if it already exists in the base code. To do this, compare the behavior against the branch the PR is based on (usually `dev`).

Even before an issue occurs, it is best practice to keep the same settings and delete the shader cache. Using an already made shader cache can make the PR look like it is having a regression in some rare cases.

Try not to test PRs which are for documentation or extremely trivial changes (like a PR that changes the app icon), unless you really want to; generally avoid PRs whose title starts with `docs:`.

If a PR specifies it is for a given platform (i.e `linux`) then just test on Linux. If it says `NCE` then test on Android and Linux ARM64 (Raspberry Pi and such). macOS fixes may also affect Asahi, test that if you can too.

Pull requests are not built automatically, so build the PR yourself (see the [build guide](../Build.md)). Be aware that your build is NOT the same as the release builds, because of package versioning and build environment differences. One famous example is FFmpeg randomly breaking on many Arch distros due to packaging differences.

## Quickstart

Think of the source code as a "tree", with the "trunk" of that tree being our `dev` branch: merged work goes there. `main` is the release branch; every push to it is built and published on the [Releases](https://github.com/notsapinho/Eglantine/releases) page. All other branches are PRs or separate development branches; they are considered unstable and aren't recommended to pull from unless you're testing multiple branches at once.

Here's some terminology you may want to familiarize yourself with:

- PR: Pull request, a change in the codebase; from which the author of said change (the programmer) requests a pull of that branch into `dev` (make it so the new code makes it into a release basically).
- Bisect: Bilinear method of searching regressions, some regressions may be sporadic and can't be bisected, but the overwhelming majority are.
- WIP: Work-in-progress.
- Regression: A new bug/glitch caused by new code, i.e "Zelda broke in android after commit xyz".
- `dev`: The "root" branch, where all merged code goes to; it contains all the code that eventually makes it to releases. `main` is the release branch, and changes reach it from `dev`.
- `HEAD`: Latest commit in a given branch, `HEAD` of `dev` is the latest commit on branch `dev`.
- `origin`: The default "remote", basically the URL from where git is located at, for most of the time that location is https://github.com/notsapinho/Eglantine.

## Testing checklist

For regressions/bugs from PRs or commits:

- [ ] Occurs on `dev`?
    - If it occurs on `dev`:
        - [ ] Occurs on the previous release? (before this particular PR).
            - If it occurs on the previous release:
                - [ ] Occurs on the release before that?
                    - And so on and so forth... some bugs come from way before Eden was even conceived.
            - Otherwise, try bisecting between the previous release AND the latest `HEAD` of `dev`
                - [ ] Occurs in given commit?
- [ ] Occurs in PR?
    - If it occurs on PR:
        - [ ] Bisected PR? (if it has commits)
        - [ ] Found bisected commit?

If an issue sporadically appears, try to do multiple runs, try if possible, to count the number of times it has failed and the number of times it has "worked just fine"; say it worked 3 times but failed 1. then there is a 1/4th chance every run that the issue is replicated - so every bisect step would require 4 runs to ensure there is at least a chance of triggering the bug.

## What to do when something seems off

If you notice something odd during testing:

- Reproduce the issue using the PR's base branch.
- Observe whether the same behavior occurs.

From there onwards there can be two possible outcomes:

- If the issue exists in the base branch: This means the problem was already present before the PR. The PR most likely did not introduce the regression.
- If the issue does not exist in the base branch: This suggests the PR most likely introduced the regression and needs further investigation.

## Reporting Your Findings

When you report your results:

- Clearly state whether the behavior was observed in the base branch.
- Indicate whether the result is good (expected behavior) or bad (unexpected or broken behavior). Without mentioning if your post/report/log is good or bad it may confuse the developer of the PR.

For example:

1. "Bad result for PR: Tested on base branch - issue not present. Likely regression introduced."
2. "Good result for PR: Tested on base branch - issue already present. Not a regression."

This approach helps maintain clarity and accountability in the testing process and ensures regressions are caught and addressed efficiently.

If the behavior seems normal for a certain game/feature then it may not be always required to check against the base branch.

If you can't build the PR's base branch, it will be helpful to just test past and future builds nearby. That would help with gathering more information about the problem.

**Always include [debugging info](../Debug.md) as needed**.

## Bisecting

One happy reminder, when testing, *know how to bisect!*

Say you're trying to find an issue between 1st of Jan and 8th of Jan, you can search by dividing "in half" the time between each commit:
- Check for 4th of Jan
- If 4th of Jan is "working" then the issue must be in the future
- So then check 6th of Jan
- If 6th of Jan isn't working then the issue must be in the past
- So then check 5th of Jan
- If 5th of Jan worked, then the issue starts at 6th of Jan

The faulty commit then, is 6th of Jan. This is called bisection https://git-scm.com/docs/git-bisect

## Notes

- PR's marked with **WIP** do NOT need to be tested unless explicitly asked (check the git in case)
- All open PRs can be viewed [here](https://github.com/notsapinho/Eglantine/pulls).
