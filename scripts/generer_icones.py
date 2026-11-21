"""Génère toutes les icônes d'Orio, son écran de démarrage et son bandeau « À propos ».

Usage : python scripts/generer_icones.py [icone.png|icone.ico]
        (icône par défaut : resources/images/orio/orio_icone.png ; pour un .ico, sa plus grande image)
Sources, rendues par scripts/logo_orio.py --png : orio_icone.png (carrée, 256 px ou plus),
orio_logo.png (logo vertical, symbole au-dessus de ORIO) et orio_logo_horizontal.png.

Écrit dans resources/images, sous les noms de Bambu Studio que le code référence :
- BambuStudio.ico : intégrée à orio.exe par BambuStudio.rc (Explorateur, barre des tâches,
  fenêtre principale) ; BambuStudioTitle.ico (barre de titre des dialogues) et
  BambuStudio-mac_256px.ico (Dock macOS) sont identiques. Images de 16 à 256 px, en PNG.
- BambuStudio_32px.png, BambuStudio_128px.png, BambuStudio-mac_128px.png ; BambuStudio.png (154 px,
  logo des boîtes de message) ; BambuStudio_192px.png et ses variantes grise et semi-transparente.
- splash_logo.png : logo vertical de l'écran de démarrage (400 px de haut, réduit par GUI_App.cpp).
- BambuStudio_about.png : bandeau « À propos » à sa taille d'affichage à 100 % (590x250).
Et resources/web/image/logo.png : logo de la page d'accueil de l'assistant de premier lancement (154 px).

Nécessite Pillow. Les réductions se font en alpha prémultiplié, sans franges sombres sur les bords.
L'icône de l'exe est intégrée à la compilation : recompiler l'application ensuite.
"""
import io
import os
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, 'resources', 'images')
ORIO = os.path.join(IMAGES, 'orio')

ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
VOID = (0x0B, 0x0F, 0x14)

SPLASH_LOGO_HEIGHT = 400

# Bandeau « À propos » affiché à 250 px de haut. Le texte de version est posé dessous à partir de
# y = 165 environ, sur le même fond VOID (AboutDialog.cpp).
BANNER_SIZE = (590, 250)
BANNER_LOGO_CENTER = (295, 88)
BANNER_LOGO_HEIGHT = 96


def load_source(path):
    im = Image.open(path)
    if im.format == 'ICO':
        im = im.ico.getimage(max(im.ico.sizes()))
    im = im.convert('RGBA')
    if im.width != im.height or im.width < 256:
        sys.exit(f'{path} : image carrée d\'au moins 256 px attendue, reçu {im.width}x{im.height}')
    return im


def load_logo(name):
    """Logo sans ses marges transparentes."""
    im = Image.open(os.path.join(ORIO, name)).convert('RGBA')
    return im.crop(im.getchannel('A').getbbox())


def resized(im, size):
    if im.size == size:
        return im.copy()
    return im.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')


def scaled(im, size):
    return resized(im, (size, size))


def to_height(im, height):
    return resized(im, (round(im.width * height / im.height), height))


def write_ico(path, images):
    """Écrit un .ico dont chaque image est stockée en PNG (format accepté depuis Windows Vista)."""
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, 'PNG', optimize=True)
        blobs.append(buf.getvalue())
    data = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        # Largeur et hauteur sur un octet : 0 signifie 256.
        data += struct.pack('<BBBBHHII', im.width % 256, im.height % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, 'wb') as fh:
        fh.write(data + b''.join(blobs))


def about_banner(logo):
    small = to_height(logo, BANNER_LOGO_HEIGHT)
    banner = Image.new('RGBA', BANNER_SIZE, VOID + (255,))
    banner.alpha_composite(small, (BANNER_LOGO_CENTER[0] - small.width // 2, BANNER_LOGO_CENTER[1] - small.height // 2))
    return banner.convert('RGB')


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ORIO, 'orio_icone.png')
    icon = load_source(source)
    frames = {size: scaled(icon, size) for size in ICO_SIZES}

    outputs = {}
    for name in ('BambuStudio.ico', 'BambuStudioTitle.ico', 'BambuStudio-mac_256px.ico'):
        write_ico(os.path.join(IMAGES, name), [frames[size] for size in ICO_SIZES])
        outputs[name] = None

    im192 = scaled(icon, 192)
    gray = im192.convert('L')
    r, g, b, a = im192.split()
    outputs.update({
        'BambuStudio_32px.png': frames[32],
        'BambuStudio_128px.png': frames[128],
        'BambuStudio-mac_128px.png': frames[128],
        'BambuStudio.png': scaled(icon, 154),
        'BambuStudio_192px.png': im192,
        'BambuStudio_192px_grayscale.png': Image.merge('RGBA', (gray, gray, gray, a)),
        'BambuStudio_192px_transparent.png': Image.merge('RGBA', (r, g, b, a.point(lambda v: v // 2))),
        'splash_logo.png': to_height(load_logo('orio_logo.png'), SPLASH_LOGO_HEIGHT),
        'BambuStudio_about.png': about_banner(load_logo('orio_logo_horizontal.png')),
        os.path.join('..', 'web', 'image', 'logo.png'): scaled(icon, 154),
    })
    for name, im in outputs.items():
        path = os.path.normpath(os.path.join(IMAGES, name))
        if im is not None:
            im.save(path, optimize=True)
        print(f'{os.path.relpath(path, ROOT)} ({os.path.getsize(path)} octets)')
    print(f'Source : {source} ({icon.width}x{icon.height})')


if __name__ == '__main__':
    main()
