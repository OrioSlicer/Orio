"""Couleurs d'Orio : le vert de Bambu (et le sarcelle d'OrcaSlicer) devient bleu, le thème sombre s'assombrit.

Usage : python scripts/couleurs_orio.py [--appliquer] [--detail]
  Sans --appliquer : compte ce qui changerait, fichier par fichier, sans rien écrire.
  Relançable : une couleur d'arrivée n'est jamais une couleur de départ. À relancer après avoir
  fusionné main dans une branche qui a ajouté du vert de Bambu ou un gris sombre de Bambu.
  Nécessite Pillow (quelques images PNG d'interface).

Seules des teintes EXACTES sont remplacées : les autres verts (axes Y, couleurs de l'Aperçu, palettes de
filaments ou de modificateurs, ColorRGB de la scène 3D) ne bougent pas.

Couleurs d'arrivée (charte graphique d'Orio) :
- BOUTON #0874E0 (texte blanc : contraste 4,6:1), survol #2B8CF2, appui #0560C2 ; pas d'entrée dans
  la table du thème sombre : les boutons restent bleu soutenu dans les deux thèmes.
- ACCENT #0877E8 (fin du dégradé d'accent de la charte) : icônes, cases, liens, bordures. En thème
  sombre, StateColor.cpp (gDarkColors), BitmapCache.cpp et ImGuiWrapper::COL_BAMBU le changent en
  ORION #00C2FF.
- Le vert sombre des icônes *_dark.svg (#21A452) devient directement ORION #00C2FF.
- Thème sombre (DARK_MAP) : les gris sombres de Bambu, environ 30 % plus près du noir. Appliqué au code
  C++, aux icônes *_dark.svg et aux feuilles dark.css seulement : le thème clair ne change pas.
Dans le code C++, un vert « normal » est un BOUTON quand son instruction parle d'appui, de survol, de
fond ou de bordure (Pressed, Hovered, SetBackgroundColo…, SetBorderColo…, btn_bg, btn_bd) ; sinon un
ACCENT. En CSS : BOUTON pour background, ACCENT sinon. ImGui : ACCENT.
"""
import colorsys
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ['src/slic3r', 'resources/images', 'resources/web']
EXT = {'.cpp', '.hpp', '.h', '.svg', '.html', '.css', '.js', '.ts', '.tsx'}
# Icônes dont le vert a un sens (échelle vert / jaune / rouge, compatible / incompatible, filament vert).
EXCLUDE = re.compile(r'^(filament_green|flag_green|flag_red|ams_humidity_\w+|hms_notify_lv\w+|monitor_signal_\w+)\.svg$')
# Lignes à ne pas toucher : palettes de couleurs distinctes (camembert du gestionnaire de filaments).
SKIP_LINE = re.compile(r'PIE_COLORS')

ACCENT, BOUTON, SURVOL, APPUI, ORION = '0877E8', '0874E0', '2B8CF2', '0560C2', '00C2FF'

# 2CAD00 : --brand du gestionnaire de filaments et de l'onglet Appareil (pages web).
# 009688 : sarcelle d'OrcaSlicer (boutons radio, calibration, icônes de menu portés d'Orca).
NORMAL = {'00AE42', '00AF42', '00B142', '00AE64', '06AE42', '00B12C', '00A854', '2CAD00', '00B35C', '00B731',
          '009688'}
HEX_MAP = {
    # survol des boutons
    '3DCB73': SURVOL, '37EE7C': SURVOL, '30DD70': SURVOL, '20E86A': SURVOL, '34C400': SURVOL,
    # appui des boutons
    '1B8844': APPUI, '17813F': APPUI, '08992E': APPUI, '009438': APPUI, '009638': APPUI,
    '009A3A': APPUI, '00A03D': APPUI, '00675B': APPUI,
    # vert du thème sombre, vert vif des pages web sur fond sombre
    '21A452': ORION, '50E81D': ORION,
    # fonds clairs (sélection, survol) ; cibles toutes différentes (clés de gDarkColors)
    'DBFDE7': 'DCEEFE', 'EBF9F0': 'EAF3FD', 'EDFAF2': 'ECF5FE', 'DBFDD5': 'DDEFFE', 'D7E8DE': 'D8E6F3',
    'C8EBD5': 'C9E2F8', 'E0FCEB': 'E1F0FE', 'E0F6E9': 'E1EFFB', 'E6F7ED': 'E6F2FC', 'DEF5E7': 'DFEFFB',
    # interrupteurs, icônes vert moyen
    '99DFB2': '9DC9F4', '6DC48D': '6CAAE9', '73CA94': '72B0EC',
    # fonds sombres (valeurs de gDarkColors, feuilles sombres, boutons ImGui des outils de peinture)
    '293F34': '1B3248', '1F3529': '142A3E', '283232': '252F3A', '1F2B27': '1C2732', '243E30': '183049',
    '205034': '10405F', '244135': '18364F', '2B4036': '1A3550',
    # survol vif des outils Mesure et Peinture multi-couleurs
    '32EE3D': SURVOL,
}
# Thème sombre plus proche du noir (remarque de l'utilisateur, build du 13-10 15h17). #2D2D30 n'y est
# pas : il sert aussi au thème clair (onglets du haut, Notebook.cpp) ; sa valeur de gDarkColors est
# changée à la main.
DARK_MAP = {
    '2D2D31': '202024', '2D2D32': '202025', '2D2D29': '20201D', '242428': '18181B', '333337': '26262A',
    '36363C': '29292E', '36363B': '29292D', '36363D': '29292F', '3B3B40': '2E2E33', '3E3E45': '313137',
    '3E3E46': '313138', '404040': '333333', '4A4A51': '3C3C42', '4C4C55': '3F3F47', '54545A': '46464C',
    '54545B': '46464D', '53545A': '45464C', '414147': '323237', '2B2B2B': '1E1E1F', '3C3C41': '2C2C31',
}
RGB = lambda h: tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

HEX_RE = re.compile(r'(?<![0-9A-Za-z])(#|0x)([0-9A-Fa-f]{6})(?![0-9A-Za-z])')
# wxColour(r, g, b), ImColor(r, g, b, a), IM_COL32(r, g, b, a), rgb(r, g, b) / rgba(r, g, b, a).
# Les séparateurs d'origine sont gardés : pas d'espace ajouté dans une classe Tailwind « bg-[rgba(…)] ».
TRIPLE_RE = re.compile(r'\b(wxColou?r|ImColor|IM_COL32|rgba?)\((\s*)(\d{1,3})(\s*,\s*)(\d{1,3})(\s*,\s*)(\d{1,3})(?=\s*[,)])')
# Vert ImGui écrit en flottants : (0.00f, 0.68f, 0.26f), (0, 0.682f, 0.259f), (0, 174.0f / 255.0f, 66.0f / 255.0f),
# et trois variantes isolées (liens des notifications, appui, survol).
FLOAT_RES = [
    (re.compile(r'\b0(?:\.0*)?f?(\s*,\s*)0\.68f(\s*,\s*)0\.26f\b'), ACCENT),
    (re.compile(r'\b0(?:\.0*)?f?(\s*,\s*)0\.682f(\s*,\s*)0\.259f\b'), ACCENT),
    (re.compile(r'\b0(?:\.0*)?f?(\s*,\s*)174\.0f?\s*/\s*255(?:\.0)?f?(\s*,\s*)66\.0f?\s*/\s*255(?:\.0)?f?'), ACCENT),
    (re.compile(r'\b0\.03(\s*,\s*)0\.6(\s*,\s*)0\.18\b'), ACCENT),
    (re.compile(r'\b0\.0f(\s*,\s*)0\.58f(\s*,\s*)0\.22f\b'), APPUI),
    (re.compile(r'\b0\.00f(\s*,\s*)0\.75f(\s*,\s*)0\.30f\b'), SURVOL),
]
# ImVec4(0 / 255.0, 174 / 255.0, 66 / 255.0, a) : couleur en entiers sur 255.
SLASH_RE = re.compile(r'\b(\d{1,3})(\s*/\s*255(?:\.0*)?f?\s*,\s*)(\d{1,3})(\s*/\s*255(?:\.0*)?f?\s*,\s*)(\d{1,3})(?=\s*/\s*255)')
BUTTON_WORDS = re.compile(r'Pressed|Hovered|SetBackgroundColo|SetBorderColo|btn_bg|btn_bd|bg_green|bd_green|BUTTON_HOVER')
CSS_BG = re.compile(r'background[\w-]*\s*:\s*[^;{}]*$')

# Images PNG d'interface (pas les captures d'écran ni les photos) : vert -> bleu, gris sombres -> DARK_MAP.
PNG = ['resources/images/plate_sheet_link_hover.png', 'resources/images/print_options_bg_dark.png',
       'resources/images/toolbar_background_dark.png', 'resources/web/guide/11/tip.png',
       'resources/web/guide/11/earth.png', 'resources/web/guide/6/restart.png', 'resources/web/login/disconnect.png',
       'resources/web/login/disconnect2.png', 'resources/web/homepage3/img/i4.png',
       'resources/images/monitor_extruder_empty_load.png', 'resources/images/monitor_extruder_empty_unload.png',
       'resources/images/monitor_extruder_filled_load.png', 'resources/images/monitor_extruder_filled_unload.png']
PNG_DARK = {(48, 47, 52): (35, 34, 38)}  # fond de print_options_bg_dark.png (en plus de DARK_MAP)


def target(hexv, button, dark):
    v = hexv.upper()
    if v in NORMAL:
        return BOUTON if button else ACCENT
    return HEX_MAP.get(v) or (DARK_MAP.get(v) if dark else None)


def statement(text, pos):
    """Instruction C++ autour de pos (du ; ou de l'accolade précédente au ; suivant)."""
    start = max(text.rfind(';', 0, pos), text.rfind('{', 0, pos), text.rfind('}', 0, pos)) + 1
    end = text.find(';', pos)
    return text[start:end if end >= 0 else len(text)]


def convert(text, name):
    ext = os.path.splitext(name)[1].lower()
    cpp = ext in ('.cpp', '.hpp', '.h')
    css = ext in ('.css', '.html')
    dark = cpp or name.endswith('_dark.svg') or name == 'dark.css'
    n = 0

    def skip(pos):
        line_start = text.rfind('\n', 0, pos) + 1
        line_end = text.find('\n', pos)
        return bool(SKIP_LINE.search(text[line_start:line_end if line_end >= 0 else len(text)]))

    def is_button(pos):
        if cpp:
            return bool(BUTTON_WORDS.search(statement(text, pos)))
        if css:
            line_start = text.rfind('\n', 0, pos) + 1
            return bool(CSS_BG.search(text[line_start:pos]))
        return False

    def hex_sub(m):
        nonlocal n
        src = m.group(2)
        t = None if skip(m.start()) else target(src, is_button(m.start()), dark)
        if not t:
            return m.group(0)
        n += 1
        return m.group(1) + (t.lower() if src != src.upper() else t)  # garde la casse d'origine

    def triple_sub(m):
        nonlocal n
        h = '%02X%02X%02X' % tuple(int(x) for x in m.group(3, 5, 7))
        t = None if skip(m.start()) else target(h, is_button(m.start()), dark)
        if not t:
            return m.group(0)
        n += 1
        r, g, b = RGB(t)
        return f'{m.group(1)}({m.group(2)}{r}{m.group(4)}{g}{m.group(6)}{b}'

    text = HEX_RE.sub(hex_sub, text)
    text = TRIPLE_RE.sub(triple_sub, text)
    if cpp:
        def scene_3d(pos):
            # ColorRGB(...) / ColorRGBA(...) : palettes de la scène 3D (supports tranchés, modificateurs).
            return bool(re.search(r'ColorRGBA?\(\s*$', text[max(0, pos - 20):pos]))

        def slash_sub(m):
            nonlocal n
            if scene_3d(m.start()):
                return m.group(0)
            t = target('%02X%02X%02X' % tuple(int(x) for x in m.group(1, 3, 5)), is_button(m.start()), dark)
            if not t:
                return m.group(0)
            n += 1
            r, g, b = RGB(t)
            return f'{r}{m.group(2)}{g}{m.group(4)}{b}'
        text = SLASH_RE.sub(slash_sub, text)
        for r, t in FLOAT_RES:
            c = RGB(t)

            def float_sub(m):
                nonlocal n
                if scene_3d(m.start()):
                    return m.group(0)
                n += 1
                return f'{c[0]}.0f / 255.0f{m.group(1)}{c[1]}.0f / 255.0f{m.group(2)}{c[2]}.0f / 255.0f'
            text = r.sub(float_sub, text)
    return text, n


def convert_png(path, apply):
    """Teinte verte -> teinte de l'accent (même saturation, plus lumineux) ; gris sombres exacts -> DARK_MAP."""
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    dark = {RGB(k): RGB(v) for k, v in DARK_MAP.items()}
    dark.update(PNG_DARK)
    hue = colorsys.rgb_to_hsv(*(x / 255 for x in RGB(ACCENT)))[0]
    px, n = im.load(), 0
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if (r, g, b) in dark:
                px[x, y] = dark[(r, g, b)] + (a,)
                n += 1
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if a and 95 <= h * 360 <= 175 and s > 0.35 and v > 0.25:
                rr, gg, bb = colorsys.hsv_to_rgb(hue, s * 0.97, min(1.0, v * 1.33))
                px[x, y] = (round(rr * 255), round(gg * 255), round(bb * 255), a)
                n += 1
    if n and apply:
        im.save(path, optimize=True)
    return n


def main():
    apply = '--appliquer' in sys.argv
    total, files = 0, 0

    def count(n, path):
        nonlocal total, files
        if n:
            total += n
            files += 1
            if '--detail' in sys.argv:
                print(f'{n:6d}  {os.path.relpath(path, ROOT)}')

    for d in DIRS:
        for dp, dn, fn in os.walk(os.path.join(ROOT, d)):
            for f in fn:
                ext = os.path.splitext(f)[1].lower()
                if ext not in EXT or EXCLUDE.match(f):
                    continue
                path = os.path.join(dp, f)
                try:
                    text = open(path, 'rb').read().decode('utf-8')
                except UnicodeDecodeError:
                    continue
                new, n = convert(text, f)
                if n and apply:
                    open(path, 'wb').write(new.encode('utf-8'))
                count(n, path)
    for rel in PNG:
        path = os.path.join(ROOT, rel)
        count(convert_png(path, apply), path)
    print(f'{total} couleurs dans {files} fichiers' + ('' if apply else ' (essai, rien écrit)'))


if __name__ == '__main__':
    main()
