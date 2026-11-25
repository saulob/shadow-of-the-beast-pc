"""Per-title facts for the game this port knows, keyed by TITLE_ID in sce_sys/param.sfo."""
from pathlib import Path

from ps4_module import parse_sfo

BASE_LAYER = {'app_ver': '01.00', 'sfo_version': '01.01', 'update': False}
MODULES = ('libc.prx', 'libSceFios2.prx')
CORE_PATTERNS = ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt')
PAK_FILES = tuple('beast/content/paks/pakchunk%d-ps4.pak' % n for n in range(1, 9))

PROFILES = {
    'CUSA03762': {
        'name': 'Shadow of the Beast',
        'region': 'Korea',
        'content_id': 'HP9000-CUSA03762_00-SHADOWOFBEAST0KR',
        'versions': {
            '1.00': {
                'layers': (BASE_LAYER,),
                'eboot_sha256': 'ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023',
                'folder': 'CUSA03762-1.00',
                'regional_archive': 'beast/content/paks/pakchunk0-ps4.pak',
                'extra_files': PAK_FILES,
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        'modules': MODULES,
        'core_patterns': CORE_PATTERNS,
    },
    'CUSA02871': {
        'name': 'Shadow of the Beast',
        'region': 'USA',
        'content_id': 'UP9000-CUSA02871_00-SHADOWOFBEAST0US',
        'versions': {
            '1.00': {
                'layers': (BASE_LAYER,),
                'eboot_sha256': 'ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023',
                'folder': 'CUSA02871-1.00',
                'regional_archive': 'beast/content/paks/pakchunk0-ps4.pak',
                'extra_files': PAK_FILES,
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        'modules': MODULES,
        'core_patterns': CORE_PATTERNS,
    },
    'CUSA00951': {
        'name': 'Shadow of the Beast',
        'region': 'Europe',
        'content_id': 'EP9000-CUSA00951_00-SHADOWOFBEAST0EU',
        'versions': {
            '1.00': {
                'layers': (BASE_LAYER,),
                'eboot_sha256': '7c35e4edc6304dc22f665182fb89385a589d04c9d4a7ac54fd8d4652e74a9343',
                'folder': 'CUSA00951-1.00',
                'regional_archive': 'beast/content/paks/pakchunk0-ps4.pak',
                'extra_files': PAK_FILES,
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        'modules': MODULES,
        'core_patterns': CORE_PATTERNS,
    },
}


def accepts_eboot(build, sha):
    """True when `sha` is the build's eboot.bin or an alternate packaging of it."""
    return sha == build['eboot_sha256'] or sha in build.get('alternate_eboots', ())


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        return parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, KeyError):
        return None


def profile(game_or_title):
    """The profile for a TITLE_ID or a game folder, or None for an unknown title."""
    key = str(game_or_title)
    if key not in PROFILES:
        key = title_id(game_or_title) or ''
    return PROFILES.get(key)


def finished_layer(build):
    """The layer whose param.sfo the installed game has."""
    return build['layers'][-1]


def layer_identity(title, layer):
    """(TITLE_ID, APP_VER, VERSION, is an update) of the package a layer is."""
    return title, layer['app_ver'], layer['sfo_version'], layer['update']


def package_identity(info):
    """The identity of a package, or None when it is not a package of a supported title."""
    sfo = info['param_sfo']
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found or sfo.get('CONTENT_ID') != found['content_id']:
        return None
    return sfo['TITLE_ID'], sfo.get('APP_VER'), sfo.get('VERSION'), sfo.get('CATEGORY') == 'gp'


def finishing_version(title, identity):
    """The version of `title` whose finished game is the package with this identity, or None."""
    for version, build in PROFILES[title]['versions'].items():
        if layer_identity(title, finished_layer(build)) == identity:
            return version
    return None


def version_of(game):
    """The profile version whose finished param.sfo matches the game folder, or None."""
    try:
        sfo = parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError):
        return None
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found:
        return None
    return next((v for v, build in found['versions'].items()
                 if (finished_layer(build)['app_ver'], finished_layer(build)['sfo_version'])
                 == (sfo.get('APP_VER'), sfo.get('VERSION'))), None)


def package_layer(info):
    """(TITLE_ID, profile version, layer index) of a finished package, or None."""
    identity = package_identity(info)
    if identity is None:
        return None
    version = finishing_version(identity[0], identity)
    if version is None:
        return None
    return identity[0], version, len(PROFILES[identity[0]]['versions'][version]['layers']) - 1
