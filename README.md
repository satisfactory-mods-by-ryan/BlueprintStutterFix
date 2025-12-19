# Blueprint Stutter Fix

A mod for Satisfactory that reduces stutter caused by having a large number of blueprints saved in your library.

## Description

When you have a lot of blueprints saved in your library, there's a noticeable stutter every 2 seconds. This happens because a loop runs every 2 seconds to verify that each blueprint has met its recipe requirements. It's fine when you don't have many saved blueprints, but with an excessive amount the loop starts to stall the game for a split second.

This mod disables that loop. It lets it run once, and then only when the ship is launched for a new milestone. If you import some blueprints and then use Advanced Game Settings to progress tiers, they might not show up at first, but saving and reloading should fix it. Eventually I could handle that more cleanly, but everything functions well as is. If there are any issues, let me know.

**NOTE:** The stutter is not caused by the number of blueprints placed in the world, only by the number of blueprints saved in your library.

## Links

Check out the mod on <a href="https://ficsit.app/mod/BlueprintStutterFix" target="_blank" rel="noopener noreferrer">ficsit.app</a> as well as <a href="https://ficsit.app/user/6CFnDcDHq3Cwf7" target="_blank" rel="noopener noreferrer">my other mods</a>.

If you have any issues or feedback, please submit <a href="https://docs.google.com/forms/d/e/1FAIpQLSeF71GZIfPfStLGFsYLMf45TdzNcc8bav4sOqB8uHf9UqiJUg/viewform?usp=pp_url&entry.848154145=Blueprint+Stutter+Fix" target="_blank" rel="noopener noreferrer" class="feedback" title="Report an issue or share feedback">this form</a>.

<a href="https://ko-fi.com/modsbyryan" target="_blank" rel="noopener noreferrer" class="kofi" title="Ryan's completely optional tip jar">Ryan's completely optional tip jar</a>

## License

This project is licensed under the [MIT License](LICENSE).
