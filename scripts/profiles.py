"""Game profiles: games/<TITLE_ID>/profile.toml, one per supported game (ps4mac_port).

A profile is data only: the title IDs it covers, its patch file, the name the port shows (window
title, data folder), environment defaults for the runtime and GPU layer, known issues and test
scenarios. The core reads it by the Title ID in the dump's param.sfo; values given in the
environment (and the settings file, for the options it holds) still win.
"""
from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

GAMES_DIR = Path(__file__).resolve().parent.parent / 'games'


@dataclass(frozen=True)
class Profile:
    folder: Path
    title: str
    title_ids: tuple[str, ...]
    match_title: str = ''          # also recognizes a release under an unlisted ID by its title
    icon: Path | None = None
    patch_file: Path | None = None
    patch_app_ver: str = ''
    modules: tuple[str, ...] = ()  # game modules the core turns on for this game ("god_of_war")
    env: dict[str, str] = field(default_factory=dict)
    known_issues: dict[str, str] = field(default_factory=dict)
    test: dict = field(default_factory=dict)

    @property
    def primary_id(self) -> str:
        return self.title_ids[0]


def _load(path: Path) -> Profile:
    with path.open('rb') as f:
        data = tomllib.load(f)
    folder = path.parent
    patches = data.get('patches', {})
    icon = data.get('icon')
    return Profile(
        folder=folder,
        title=data['title'],
        title_ids=tuple(data['title_ids']),
        match_title=data.get('match_title', ''),
        icon=folder / icon if icon else None,
        patch_file=folder / patches['file'] if patches.get('file') else None,
        patch_app_ver=patches.get('app_ver', ''),
        modules=tuple(data.get('modules', ())),
        env={k: str(v) for k, v in data.get('env', {}).items()},
        known_issues=dict(data.get('known_issues', {})),
        test=dict(data.get('test', {})),
    )


def all_profiles() -> list[Profile]:
    return [_load(p) for p in sorted(GAMES_DIR.glob('*/profile.toml'))]


def find(title_id: str | None, title: str | None = None) -> Profile | None:
    """The profile covering a Title ID, or whose match_title is in the game's title."""
    title = (title or '').replace('®', '').replace('™', '')
    for profile in all_profiles():
        if title_id in profile.title_ids or (title and profile.match_title and profile.match_title in title):
            return profile
    return None


def default_game_dir(port: Path) -> Path | None:
    """A dump folder named after a profiled Title ID next to the port or one level further."""
    for profile in all_profiles():
        for title_id in profile.title_ids:
            for candidate in [port.parent / title_id, *sorted(port.parent.glob(f'*/{title_id}'))]:
                if (candidate / 'eboot.bin').is_file():
                    return candidate
    return None
