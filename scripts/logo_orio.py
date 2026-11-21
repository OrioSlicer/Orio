"""Dessine le logo d'Orio en SVG (charte graphique d'Orio).

Usage : python scripts/logo_orio.py [--png]
  Écrit les SVG dans resources/images/orio/. Avec --png, rend aussi (par Microsoft Edge sans fenêtre) les
  PNG dont scripts/generer_icones.py tire les icônes, l'écran de démarrage et le bandeau « À propos ».

Symbole, redessiné d'après l'image de la charte : un anneau (trou = 64 % du diamètre) coupé par une
bande diagonale à 37,5°. Le bord blanc de la bande passe par le centre (demi-anneau blanc exact) ;
le bord cyan est décalé de 24,5 % du rayon, d'où un arc cyan plus court.
Mot-symbole ORIO : capitales fines et très espacées, O en rectangle très arrondi ; dessiné en traits,
sans police, pour rester identique partout.
Uniquement des chemins simples (ni masque ni découpe) : lisible par nanosvg, dans l'appli.
"""
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'resources', 'images', 'orio')

ORION, VOID, LUMIERE = '#00C2FF', '#0B0F14', '#E6F0FF'

INNER = 0.64        # rayon intérieur / rayon extérieur
ANGLE = 37.5        # direction de la bande, en degrés depuis l'horizontale
CYAN_OFFSET = 0.245  # écart entre les deux bords de la bande, en rayons

# Mot-symbole, en centièmes de la hauteur des capitales.
STROKE = 18
O_WIDTH, O_RADIUS = 148, 46
R_WIDTH, R_BOWL, R_BOWL_RADIUS, R_LEG_TOP = 124, 57, 24, 72
I_WIDTH = STROKE
SPACING = 110


def f(v):
    return f'{v:.2f}'.rstrip('0').rstrip('.')


def ring_piece(cx, cy, R, c, cyan_side):
    """Morceau de l'anneau d'un côté de la droite n·p = c (n vers le bas à droite, côté cyan)."""
    a = math.radians(ANGLE)
    u = (math.cos(a), -math.sin(a))   # le long de la bande (repère SVG, y vers le bas)
    n = (math.sin(a), math.cos(a))    # perpendiculaire, vers le cyan
    r = INNER

    def pt(t):
        return f(cx + R * (c * n[0] + t * u[0])), f(cy + R * (c * n[1] + t * u[1]))

    t_out, t_in = math.sqrt(1 - c * c), math.sqrt(r * r - c * c)
    # Ouverture des arcs : côté cyan, l'arc tourne vers n (moins de 180°) ; côté blanc, à l'opposé.
    k = c if cyan_side else -c
    large_out = 0 if 2 * math.acos(k) <= math.pi else 1
    large_in = 0 if 2 * math.acos(k / r) <= math.pi else 1
    sweep_out = 1 if cyan_side else 0
    (ax, ay), (bx, by) = pt(t_out), pt(-t_out)
    (cx_, cy_), (dx, dy) = pt(-t_in), pt(t_in)
    Ro, Ri = f(R), f(R * r)
    return (f'M{ax} {ay}A{Ro} {Ro} 0 {large_out} {sweep_out} {bx} {by}'
            f'L{cx_} {cy_}A{Ri} {Ri} 0 {large_in} {1 - sweep_out} {dx} {dy}Z')


def symbol(cx, cy, R, white, cyan):
    return (f'<path fill="{white}" d="{ring_piece(cx, cy, R, 0.0, False)}"/>'
            f'<path fill="{cyan}" d="{ring_piece(cx, cy, R, CYAN_OFFSET, True)}"/>')


def wordmark_width():
    return 2 * O_WIDTH + R_WIDTH + I_WIDTH + 3 * SPACING


def wordmark(x, y, H, color):
    """ORIO, coin haut gauche (x, y), hauteur des capitales H."""
    k = H / 100
    s = STROKE * k
    h = s / 2

    def O(x0):
        return (f'<rect x="{f(x0 + h)}" y="{f(y + h)}" width="{f(O_WIDTH * k - s)}" height="{f(H - s)}" '
                f'rx="{f(O_RADIUS * k - h)}"/>')

    xr = x + (O_WIDTH + SPACING) * k
    rb = R_BOWL_RADIUS * k
    right = xr + R_WIDTH * k - h
    bowl = y + R_BOWL * k
    r_path = (f'<path d="M{f(xr + h)} {f(y + H)}V{f(y + h)}H{f(right - rb)}'
              f'A{f(rb)} {f(rb)} 0 0 1 {f(right)} {f(y + h + rb)}V{f(bowl - rb)}'
              f'A{f(rb)} {f(rb)} 0 0 1 {f(right - rb)} {f(bowl)}H{f(xr + h)}"/>')
    # Jambe du R : parallélogramme à base horizontale, son haut caché dans le trait de la boucle.
    leg_top, leg_bottom = xr + R_LEG_TOP * k, xr + R_WIDTH * k
    wl = s
    for _ in range(8):  # largeur horizontale donnant une épaisseur perpendiculaire égale au trait
        wl = s * math.hypot(1, (leg_bottom - leg_top - wl) / (y + H - bowl))
    leg = (f'<path stroke="none" fill="{color}" d="M{f(leg_top)} {f(bowl)}H{f(leg_top + wl)}'
           f'L{f(leg_bottom)} {f(y + H)}H{f(leg_bottom - wl)}Z"/>')
    xi = xr + (R_WIDTH + SPACING) * k
    i_rect = f'<rect x="{f(xi)}" y="{f(y)}" width="{f(I_WIDTH * k)}" height="{f(H)}" stroke="none" fill="{color}"/>'
    xo2 = xi + (I_WIDTH + SPACING) * k
    return (f'<g fill="none" stroke="{color}" stroke-width="{f(s)}" stroke-linejoin="round">'
            f'{O(x)}{r_path}{leg}{i_rect}{O(xo2)}</g>')


def svg(w, h, body, bg=None):
    rect = f'<rect width="{w}" height="{h}" fill="{bg}"/>' if bg else ''
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'{rect}{body}</svg>\n')


def files():
    out = {}
    # Symbole seul : 512 px, rayon 200.
    for name, white, cyan in (('orio_symbole', LUMIERE, ORION), ('orio_symbole_clair', VOID, ORION),
                              ('orio_symbole_blanc', '#FFFFFF', '#FFFFFF'), ('orio_symbole_noir', '#000000', '#000000')):
        out[name] = svg(512, 512, symbol(256, 256, 200, white, cyan))

    # Icône de l'application : carré arrondi VOID (lisible sur barre des tâches claire ou sombre).
    out['orio_icone'] = svg(1024, 1024, f'<rect width="1024" height="1024" rx="224" fill="{VOID}"/>'
                                        + symbol(512, 512, 340, LUMIERE, ORION))

    # Logo vertical (comme l'image de la charte) : symbole au-dessus de ORIO.
    R = 200
    H = 0.362 * R
    ww = wordmark_width() * H / 100
    w = max(2 * R, ww) + 2 * 72
    top = 72
    gap = 0.57 * R
    h = top + 2 * R + gap + H + 72
    for name, white, cyan, text in (('orio_logo', LUMIERE, ORION, LUMIERE), ('orio_logo_clair', VOID, ORION, VOID)):
        out[name] = svg(round(w), round(h), symbol(w / 2, top + R, R, white, cyan)
                        + wordmark((w - ww) / 2, top + 2 * R + gap, H, text))

    # Logo horizontal (bandeaux larges) : symbole à gauche, ORIO à droite.
    H = 0.5 * R
    ww = wordmark_width() * H / 100
    gap = 0.75 * R
    w = 72 + 2 * R + gap + ww + 72
    h = 2 * R + 2 * 72
    for name, white, cyan, text in (('orio_logo_horizontal', LUMIERE, ORION, LUMIERE),
                                    ('orio_logo_horizontal_clair', VOID, ORION, VOID)):
        out[name] = svg(round(w), round(h), symbol(72 + R, h / 2, R, white, cyan)
                        + wordmark(72 + 2 * R + gap, (h - H) / 2, H, text))
    return out


def edge_png(svg_path, png_path, w, h):
    edge = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, 'page.html')
        with open(page, 'w', encoding='utf-8') as fh:
            fh.write(f'<html><body style="margin:0;background:transparent">'
                     f'<img src="file:///{svg_path.replace(os.sep, "/")}" width="{w}" height="{h}"></body></html>')
        subprocess.run([edge, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--user-data-dir={tmp}',
                        '--default-background-color=00000000', f'--window-size={w},{h}',
                        f'--screenshot={png_path}', 'file:///' + page.replace(os.sep, '/')],
                       check=True, capture_output=True, timeout=120)


# Sources PNG de scripts/generer_icones.py : icône, logo vertical (écran de démarrage), logo
# horizontal (« À propos »). Les logos sont rendus au double de la taille de leur SVG.
PNG_SOURCES = {'orio_icone': 1, 'orio_logo': 2, 'orio_logo_horizontal': 2}


def main():
    os.makedirs(OUT, exist_ok=True)
    svgs = files()
    for name, text in svgs.items():
        with open(os.path.join(OUT, name + '.svg'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        print(f'resources/images/orio/{name}.svg')
    if '--png' in sys.argv:
        for name, scale in PNG_SOURCES.items():
            w, h = (int(v) * scale for v in re.search(r'width="(\d+)" height="(\d+)"', svgs[name]).groups())
            png = os.path.join(OUT, name + '.png')
            edge_png(os.path.join(OUT, name + '.svg'), png, w, h)
            print(f'resources/images/orio/{name}.png ({w}x{h}, {os.path.getsize(png)} octets)')


if __name__ == '__main__':
    main()
