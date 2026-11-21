"""Catalogue des imprimantes et des filaments de Orio.

Réunit dans resources/profiles les profils de Bambu Studio et d'OrcaSlicer, au format lu par Orio
(lot 1 ; PrusaSlicer, converti depuis ses fichiers .ini, viendra compléter ce qu'Orca n'a pas).

Usage :
    python scripts/catalogue_profils.py --bambu <BambuStudio>/resources/profiles --orca <OrcaSlicer>/resources/profiles
    python scripts/catalogue_profils.py --verifier        (vérifie seulement resources/profiles)

Priorités (choix de l'utilisateur, 12-10-2026) : les imprimantes Bambu Lab viennent de Bambu Studio, telles
quelles ; toutes les autres marques viennent d'OrcaSlicer, converties ; pas d'imprimantes résine.

Le dossier resources/profiles est entièrement réécrit, puis vérifié comme le ferait le chargeur de Orio
(PresetBundle::load_vendor_configs_from_json, WebGuideDialog::LoadProfileFamily) : un seul fichier fautif y fait
sauter toute une marque. Le bilan (marques, exclusions et leur raison) est écrit dans le fichier --bilan.
"""
import argparse
import collections
import json
import os
import re
import shutil
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILS = os.path.join(RACINE, 'resources', 'profiles')
BBL = 'BBL'
BIBLIOTHEQUE = 'OrcaFilamentLibrary'   # filaments d'OrcaSlicer partagés par toutes les marques (sauf Bambu Lab)

# Révision de la conversion : à augmenter à chaque nouvelle livraison du catalogue, pour que les profils déjà
# copiés chez l'utilisateur (dossier system/) soient remplacés (PresetUpdater compare les versions).
REVISION = 1   # 1 : G-code binaire des Prusa (binary_gcode)

# Codes d'imprimante Prusa (M862.3 P "…") dont le firmware lit le G-code binaire : PrusaSlicer y met binary_gcode = 1
MODELES_BGCODE = {'MINI', 'MK3.5', 'MK3.9', 'MK4', 'MK4S', 'XL', 'COREONE', 'COREONEL'}

MARQUES_ECARTEES = {
    BBL: "imprimantes Bambu Lab : prises dans Bambu Studio",
    'OrcaArena': "copie d'OrcaSlicer de la Bambu Lab X1 Carbon, déjà fournie par Bambu Studio",
    'Ginger Additive': "imprimante à granulés : Orio ne gère pas l'extrusion de granulés",
}

# Clés d'OrcaSlicer qui existent dans Orio sous un autre nom (même sens).
CLES_RENOMMEES = {
    'chamber_temperature': 'chamber_temperatures',
    'role_based_wipe_speed': 'role_base_wipe_speed',
    'extruder_clearance_radius': 'extruder_clearance_max_radius',
    'first_layer_flow_ratio': 'initial_layer_flow_ratio',
    'top_surface_flow_ratio': 'top_solid_infill_flow_ratio',
}

# Valeurs d'énumération d'OrcaSlicer -> valeurs de Orio.
MOTIFS = ('sparse_infill_pattern', 'top_surface_pattern', 'bottom_surface_pattern', 'internal_solid_infill_pattern',
          'ironing_pattern', 'support_ironing_pattern')
VALEURS = {
    'ensure_vertical_shell_thickness': {'none': 'disabled', 'ensure_critical_only': 'partial',
                                        'ensure_moderate': 'partial', 'ensure_all': 'enabled'},
    'seam_position': {'aligned_back': 'aligned'},
    'support_style': {'organic': 'tree_organic'},
    'overhang_threshold_participating_cooling': {'95%': '100%'},
}
for _k in MOTIFS:
    VALEURS[_k] = {'rectilinear': 'zig-zag'}

# Options-listes qui peuvent rester vides (ailleurs, une liste vide fait planter le tranchage : default_filament_colour).
LISTES_VIDES_PERMISES = {'compatible_printers', 'compatible_prints', 'post_process', 'upward_compatible_machine',
                         'default_filament_profile', 'extruder_printable_area', 'extruder_printable_height',
                         'renamed_from', 'different_settings_to_system', 'thumbnail_size'}
# Valeurs anciennes acceptées par Orio à la lecture (PrintConfigDef::handle_legacy).
VALEURS_HERITEES = {
    'support_style': {'grid', 'snug'},
    'support_type': {'normal', 'tree', 'normal(manual)', 'tree(manual)', 'hybrid(auto)'},
    'support_base_pattern': {'none'},
    'ensure_vertical_shell_thickness': {'0', '1'},
    'filament_map_mode': {'Auto'},
}

# Variables de G-code d'OrcaSlicer / PrusaSlicer que Orio fournit (GCode.cpp, « Orio: variables »).
VARIABLES_AJOUTEES = {
    'first_layer_height', 'temperature', 'retract_length', 'position', 'is_extruder_used', 'idle_temperature',
    'extruded_weight_total', 'adaptive_bed_mesh_min', 'adaptive_bed_mesh_max', 'bed_mesh_probe_count',
    'bed_mesh_algo',
    # options d'imprimante du maillage adaptatif d'OrcaSlicer (PrintConfig.cpp)
    'bed_mesh_min', 'bed_mesh_max', 'bed_mesh_probe_distance', 'adaptive_bed_mesh_margin',
}
# Variables de G-code renommées (nom PrusaSlicer / OrcaSlicer -> nom Orio), ou fixées à une constante
# quand la fonction n'existe pas dans Orio (têtes parallèles de l'Elegoo OrangeStorm Giga : une seule tête).
VARIABLES_REMPLACEES = {
    'output_filename_format': 'filename_format',
    'parallel_printheads_count': '1',
}
# Réécritures de G-code : options listes chez OrcaSlicer, simples chez Orio (notes du filament).
REECRITURES = [
    (r'(?<![\w.])filament_notes\s*\[[^\]]*\]', 'filament_notes'),
]
# Variables fixées par PlaceholderParser lui-même ou ses appelants hors PrintConfig.
VARIABLES_PARSEUR = {
    'timestamp', 'year', 'month', 'day', 'hour', 'minute', 'second', 'version', 'input_filename',
    'input_filename_base', 'current_extruder', 'zhop', 'scale', 'num_extruders', 'printer_preset', 'print_preset',
    'filament_preset', 'physical_printer_preset',
}
MOTS_CLES = {'if', 'elsif', 'else', 'endif', 'and', 'or', 'not', 'true', 'false', 'min', 'max', 'random', 'int',
             'round', 'ceil', 'floor', 'digits', 'zdigits', 'filament_change'}
SYNTAXE_RECENTE = {'local', 'global', 'one_of', 'is_nil', 'interpolate_table', 'size', 'empty', 'repeat'}

META = {'type', 'name', 'inherits', 'from', 'setting_id', 'filament_id', 'instantiation', 'is_custom_defined',
        'version', 'description', 'url', 'include'}
LISTES = ('machine_model_list', 'process_list', 'filament_list', 'machine_list')
CHAMPS_MODELE = ('name', 'model_id', 'nozzle_diameter', 'machine_tech', 'family', 'bed_model', 'default_bed_type',
                 'bed_texture', 'hotend_model', 'default_materials', 'not_support_bed_type', 'image_bed_type')


# --- définitions de Orio ----------------------------------------------------------------------------
def lire_definitions():
    """Options de Orio (PrintConfig.cpp) : {clé: (type, valeurs d'énumération)} + variables de G-code."""
    src = open(os.path.join(RACINE, 'src', 'libslic3r', 'PrintConfig.cpp'), encoding='utf-8', errors='replace').read()
    defs, bornes = {}, {}
    for bloc in re.split(r'(?=\b(?:def\s*=\s*)?this->add(?:_nullable)?\(\s*")', src):
        m = re.match(r'(?:def\s*=\s*)?this->add(?:_nullable)?\(\s*"(\w+)"\s*,\s*(co\w+)', bloc)
        if m:
            defs[m.group(1)] = (m.group(2), re.findall(r'enum_values\.(?:push_back|emplace_back)\(\s*"([^"]*)"', bloc))
            mi = re.search(r'def->min\s*=\s*(-?[\d.]+)\s*;', bloc)
            ma = re.search(r'def->max\s*=\s*(-?[\d.]+)\s*;', bloc)
            if mi or ma:
                bornes[m.group(1)] = (float(mi.group(1)) if mi else None, float(ma.group(1)) if ma else None)
    for ax in 'xyze':
        for k in ('machine_max_speed_', 'machine_max_acceleration_', 'machine_max_jerk_'):
            defs.setdefault(k + ax, ('coFloats', []))
    for liste in re.findall(r'const std::vector<std::string>\s+filament_\w*override_keys\s*=\s*\{(.*?)\};', src, re.S):
        for k in re.findall(r'"(\w+)"', re.sub(r'//[^\n]*', '', liste)):
            defs.setdefault(k, defs.get(k[len('filament_'):], ('?', [])))
    for k in re.findall(r'\badd_\w+\(\s*"(\w+)"', src):
        defs.setdefault(k, ('?', []))
    variables = set(VARIABLES_PARSEUR)
    for racine, _, fichiers in os.walk(os.path.join(RACINE, 'src', 'libslic3r')):
        for f in fichiers:
            if f.endswith(('.cpp', '.hpp')):
                t = open(os.path.join(racine, f), encoding='utf-8', errors='replace').read()
                variables |= set(re.findall(r'(?:\.set|->set|set_key_value)\(\s*"(\w+)"', t))
    return defs, variables, bornes


# --- lecture / écriture -------------------------------------------------------------------------------------
def lire(p):
    with open(p, encoding='utf-8-sig') as f:
        return json.load(f)


def ecrire(p, j):
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(j, f, ensure_ascii=False, indent=4)
        f.write('\n')


class Marque:
    """Une marque (fichier <Marque>.json + dossier <Marque>/) d'un dossier profiles."""

    def __init__(self, racine, nom):
        self.racine, self.nom = racine, nom
        self.index = lire(os.path.join(racine, nom + '.json'))
        self.fichiers = {}   # (liste, nom) -> json
        for liste in LISTES:
            for e in self.index.get(liste, []):
                self.fichiers[(liste, e['name'])] = lire(os.path.join(racine, nom, e['sub_path']))

    def chemin(self, liste, nom):
        for e in self.index.get(liste, []):
            if e['name'] == nom:
                return os.path.join(self.racine, self.nom, e['sub_path'])

    def resoudre(self, liste, nom, autre=None, meta=False, _vus=None):
        """Configuration fusionnée le long de « inherits » (et « include »). autre : marque de secours."""
        _vus = _vus or set()
        if nom in _vus:
            raise ValueError('héritage circulaire sur ' + nom)
        _vus.add(nom)
        j, src = self.fichiers.get((liste, nom)), self
        if j is None and autre is not None:
            j, src = autre.fichiers.get((liste, nom)), autre
        if j is None:
            raise KeyError(nom)
        out = {}
        if j.get('inherits'):
            out = src.resoudre(liste, j['inherits'], autre if src is self else None, meta, _vus)
        inc = j.get('include', [])
        for i in ([inc] if isinstance(inc, str) else inc):
            if (liste, i) in src.fichiers:
                out.update({k: v for k, v in src.fichiers[(liste, i)].items() if k not in META})
        out.update({k: v for k, v in j.items() if meta and k in ('filament_id',) or k not in META})
        return out


def marques_de(racine):
    return sorted(f[:-5] for f in os.listdir(racine)
                  if f.endswith('.json') and os.path.isdir(os.path.join(racine, f[:-5])))


# --- G-code ---------------------------------------------------------------------------------------------------
_chaine = re.compile(r'"(?:\\.|[^"\\])*"')
_regex = re.compile(r'[=!]~\s*/(?:\\.|[^/\\])*/')
_ident = re.compile(r'(?<![\w.])([A-Za-z_]\w*)')


def blocs(texte):
    """Contenu des blocs {…} d'un modèle de G-code, chaînes et expressions régulières retirées."""
    return [_regex.sub(' ', _chaine.sub(' ', b)) for b in re.findall(r'\{([^{}]*)\}', texte)]


def identifiants(texte):
    found = set()
    for b in blocs(texte):
        found |= {m.group(1) for m in _ident.finditer(b)}
    found |= set(re.findall(r'\[([A-Za-z_]\w*)\]', texte))
    return found


def syntaxe_recente(texte):
    """Mots ou affectations du langage de macros récent (PrusaSlicer 2.6+), que Orio ne lit pas."""
    trouve = set()
    for b in blocs(texte):
        trouve |= {m.group(1) for m in _ident.finditer(b)} & SYNTAXE_RECENTE
        if re.search(r'(?<![=!<>])=(?![=~])', b) or ';' in b:
            trouve.add('affectation')
    # hors des blocs {…}, tout « [ » ouvre une variable (PlaceholderParser : lit('[') > legacy_variable_expansion)
    hors_blocs = texte
    while True:
        t = re.sub(r'\{[^{}]*\}', '', hors_blocs)
        if t == hors_blocs:
            break
        hors_blocs = t
    if re.search(r'\[(?![A-Za-z_]\w*(?:\[[A-Za-z_]\w*\])?\])', hors_blocs):
        trouve.add('crochet hors variable')
    return trouve


def reecrire_interpolate_table(texte):
    """{interpolate_table(x, (x0,y0), (x1,y1), …)} -> chaîne de {if}/{elsif} équivalente (interpolation linéaire)."""
    def remplace(m):
        corps = m.group(1)
        args, prof, cur = [], 0, ''
        for c in corps:
            if c == ',' and prof == 0:
                args.append(cur.strip())
                cur = ''
                continue
            prof += c == '('
            prof -= c == ')'
            cur += c
        args.append(cur.strip())
        x, pts = args[0], []
        for a in args[1:]:
            mm = re.fullmatch(r'\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)', a)
            if not mm:
                return m.group(0)
            pts.append((float(mm.group(1)), float(mm.group(2))))
        if len(pts) < 2:
            return m.group(0)
        g = lambda v: ('%f' % v).rstrip('0').rstrip('.')
        out = '{if %s <= %s}%s' % (x, g(pts[0][0]), g(pts[0][1]))
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            out += '{elsif %s <= %s}{%s + (%s - %s) * %s}' % (x, g(x1), g(y0), x, g(x0), g((y1 - y0) / (x1 - x0)))
        return out + '{else}%s{endif}' % g(pts[-1][1])
    return re.sub(r'\{\s*interpolate_table\((.*?)\)\s*\}', remplace, texte)


def reecrire_gcode(texte):
    if not isinstance(texte, str):
        return texte
    for ancien, nouveau in VARIABLES_REMPLACEES.items():
        if nouveau.isdigit():   # constante : [x] -> 1, puis {… x …} -> {… 1 …}
            texte = texte.replace('[%s]' % ancien, nouveau)
        texte = re.sub(r'(?<![\w.])%s(?!\w)' % ancien, nouveau, texte)
    for motif, remplacement in REECRITURES:
        texte = re.sub(motif, remplacement, texte)
    return reecrire_interpolate_table(texte)


# --- conversion d'OrcaSlicer -----------------------------------------------------------------------------------
class Convertisseur:
    def __init__(self, defs, variables, journal):
        self.defs, self.variables, self.journal = defs, variables, journal
        self.cles_gcode = {k for k in defs if k.endswith('gcode')}
        self.garder_multi = False

    def valeur_valide(self, cle, val):
        typ, enum = self.defs.get(cle, ('?', []))
        if typ not in ('coEnum', 'coEnums', 'coEnumsNullable') or not enum:
            return True
        vals = val if isinstance(val, list) else [val]
        return all(x in enum or x == 'nil' or x in VALEURS_HERITEES.get(cle, ()) for v in vals for x in str(v).split(','))

    def convertir_fichier(self, j, marque, chemin):
        """Renomme les clés, convertit les valeurs, réécrit le G-code. Modifie j sur place."""
        for ancien, nouveau in CLES_RENOMMEES.items():
            if ancien in j:
                v = j.pop(ancien)
                j.setdefault(nouveau, v)
        if 'only_one_wall_top' in j and 'top_one_wall_type' not in j:
            v = j['only_one_wall_top']
            v = v[0] if isinstance(v, list) else v
            j['top_one_wall_type'] = 'all top' if str(v) in ('1', 'true') else 'not apply'
        if 'thumbnails' in j and 'thumbnail_size' not in j:
            t = j['thumbnails']
            tailles = re.findall(r'(\d+x\d+)', ','.join(t) if isinstance(t, list) else str(t))
            if tailles:
                j['thumbnail_size'] = tailles
        for cle in list(j):
            if cle in META:
                continue
            val = j[cle]
            typ = self.defs.get(cle, ('?',))[0]
            if val in ('', []) and (typ.endswith('s') or typ.endswith('sNullable')) and cle not in LISTES_VIDES_PERMISES \
                    and not typ.startswith('coPoints'):   # zones (plateau, détection…) : vide = annule celle du parent
                # liste vide là où Orio attend une valeur par filament / extrudeur : le tranchage plante
                self.journal['valeurs retirées'][(cle, repr(val))] += 1
                del j[cle]
                continue
            if cle in VALEURS:
                conv = lambda x: ','.join(VALEURS[cle].get(y, y) for y in str(x).split(','))
                val = j[cle] = [conv(x) for x in val] if isinstance(val, list) else conv(val)
            if not self.valeur_valide(cle, val):
                self.journal['valeurs retirées'][(cle, str(val))] += 1
                del j[cle]
                continue
            if cle in self.cles_gcode:
                j[cle] = [reecrire_gcode(x) for x in val] if isinstance(val, list) else reecrire_gcode(val)

    def gcode_inconnu(self, cfg):
        """Problèmes de G-code d'une configuration résolue : syntaxe récente, variables inconnues."""
        pb = set()
        for cle, val in cfg.items():
            if cle not in self.cles_gcode:
                continue
            for t in (val if isinstance(val, list) else [val]):
                if not isinstance(t, str):
                    continue
                pb |= syntaxe_recente(t)
                for b in blocs(t):   # option simple chez Orio employée comme une liste : x[…]
                    for mm in re.finditer(r'(?<![\w.])([A-Za-z_]\w*)\s*\[', b):
                        typ = self.defs.get(mm.group(1), ('?',))[0]
                        if mm.group(1) in self.defs and not (typ.endswith('s') or typ.endswith('sNullable') or typ in ('coPoint', 'coPoints')):
                            pb.add('%s indexée' % mm.group(1))
                connu = lambda x: x in self.defs or x in self.variables or x in VARIABLES_AJOUTEES
                for ident in identifiants(t):
                    if ident in MOTS_CLES or ident in SYNTAXE_RECENTE or connu(ident) or connu(re.sub(r'_\d+$', '', ident)):
                        continue
                    pb.add(ident)
        return pb


LARGEURS = ('line_width', 'outer_wall_line_width', 'inner_wall_line_width', 'top_surface_line_width',
            'sparse_infill_line_width', 'internal_solid_infill_line_width', 'support_line_width',
            'initial_layer_line_width', 'skin_infill_line_width', 'skeleton_infill_line_width')
SURPLOMBS = ('overhang_1_4_speed', 'overhang_2_4_speed', 'overhang_3_4_speed', 'overhang_4_4_speed')
TYPES_BORNES = ('coFloat', 'coFloats', 'coInt', 'coInts', 'coPercent', 'coPercents')


def nombre(v):
    v = v[0] if isinstance(v, list) and v else v
    try:
        return float(str(v).strip().rstrip('%'))
    except ValueError:
        return None


def texte_nombre(x, entier=False):
    return str(int(round(x))) if entier else ('%.4f' % x).rstrip('0').rstrip('.')


def buse_du_reglage(cfg, nom, buses):
    """Diamètre de buse d'un réglage : imprimantes compatibles, sinon condition, sinon nom (« … 0.6 nozzle »)."""
    d = {buses[p] for p in cfg.get('compatible_printers', []) if buses.get(p)}
    if len(d) == 1:
        return d.pop()
    m = re.search(r'nozzle_diameter\[0\]\s*==\s*([\d.]+)', str(cfg.get('compatible_printers_condition', '')))
    if m:
        return float(m.group(1))
    m = re.search(r'(\d\.\d+)\s*(?:mm\s*)?nozzle', nom, re.I) or re.search(r'@.*?(0\.\d+)\s*$', nom)
    if m:
        return float(m.group(1))
    return min(d) if d else None


def ajuster_nombres(m, lib, conv, journal):
    """Ce qu'OrcaSlicer comprend et pas Orio : largeurs de ligne en % de la buse et vitesses de surplomb en %
    de celle des parois extérieures -> valeurs absolues ; valeurs hors des bornes de Orio -> ramenées dedans."""
    buses = {}
    for e in m.index.get('machine_list', []):
        if m.fichiers[('machine_list', e['name'])].get('instantiation') == 'true':
            buses[e['name']] = nombre(m.resoudre('machine_list', e['name']).get('nozzle_diameter', '0.4'))
    for liste in ('process_list', 'filament_list', 'machine_list'):
        for e in m.index.get(liste, []):
            j = m.fichiers[(liste, e['name'])]
            if j.get('instantiation') != 'true':
                continue
            cfg = m.resoudre(liste, e['name'], lib if liste == 'filament_list' and m.nom != BIBLIOTHEQUE else None)
            if liste == 'process_list':
                buse = None
                for k in LARGEURS:
                    v = cfg.get(k)
                    if isinstance(v, str) and v.strip().endswith('%'):
                        buse = buse or buse_du_reglage(cfg, e['name'], buses)
                        if buse is None:
                            j[k] = '0'   # 0 : largeur par défaut de la buse
                            journal['pourcentages sans buse'][(m.nom, e['name'])] += 1
                        else:
                            j[k] = texte_nombre(nombre(v) / 100 * buse)
                        cfg[k] = j[k]
                        journal['pourcentages convertis'][k] += 1
                v = cfg.get('support_object_xy_distance')   # % de la largeur des parois extérieures (PrusaSlicer)
                if isinstance(v, str) and v.strip().endswith('%'):
                    buse = buse or buse_du_reglage(cfg, e['name'], buses) or 0.4
                    largeur = nombre(cfg.get('outer_wall_line_width')) or nombre(cfg.get('line_width')) or 1.125 * buse
                    j['support_object_xy_distance'] = cfg['support_object_xy_distance'] = texte_nombre(nombre(v) / 100 * largeur)
                    journal['pourcentages convertis']['support_object_xy_distance'] += 1
                ref = nombre(cfg.get('outer_wall_speed'))
                for k in SURPLOMBS:
                    v = cfg.get(k)
                    vals = v if isinstance(v, list) else [v]
                    if ref and any(str(x).strip().endswith('%') for x in vals):
                        out = [texte_nombre(nombre(x) / 100 * ref) if str(x).strip().endswith('%') else x for x in vals]
                        j[k] = cfg[k] = out if isinstance(v, list) else out[0]
                        journal['pourcentages convertis'][k] += 1
            for k, v in list(cfg.items()):
                typ = conv.defs.get(k, ('?',))[0]
                if typ not in TYPES_BORNES or k not in conv.bornes:
                    continue
                mi, ma = conv.bornes[k]
                vals = v if isinstance(v, list) else [v]
                out, change = [], False
                for x in vals:
                    n = nombre(x)
                    if n is None or str(x).strip() == 'nil':
                        out.append(x)
                        continue
                    c = min(max(n, mi if mi is not None else n), ma if ma is not None else n)
                    if c != n:
                        change = True
                        journal['valeurs ramenées'][(k, str(x), texte_nombre(c, typ.startswith('coInt')))] += 1
                        out.append(texte_nombre(c, typ.startswith('coInt')) + ('%' if str(x).strip().endswith('%') else ''))
                    else:
                        out.append(x)
                if change:
                    j[k] = out if isinstance(v, list) else out[0]


def zone_en_cadre(valeur):
    """Zone exclue dessinée comme un cadre (contour + trou en un seul polygone, Anycubic Kobra 3) : Orio
    la lit comme tout le plateau exclu et refuse de trancher."""
    try:
        pts = [tuple(float(c) for c in str(p).split('x')) for p in (valeur if isinstance(valeur, list) else [valeur])]
    except ValueError:
        return False
    boucles, debut = [], 0
    for i in range(1, len(pts)):
        if pts[i] == pts[debut] and i - debut >= 3:
            boucles.append(pts[debut:i])
            debut = i + 1
    bb = lambda b: (min(p[0] for p in b), min(p[1] for p in b), max(p[0] for p in b), max(p[1] for p in b))
    for a in boucles:
        for b in boucles:
            A, B = bb(a), bb(b)
            if a is not b and A[0] < B[0] and A[1] < B[1] and A[2] > B[2] and A[3] > B[3]:
                return True
    return False


def version_orio(v):
    """02.04.00.02 -> 02.04.<50+REVISION>.02 : chaque livraison du catalogue remplace la copie installée."""
    p = (v.split('.') + ['0', '0', '0', '0'])[:4]
    return '%02d.%02d.%02d.%02d' % (int(p[0]), int(p[1]), 50 + REVISION, int(p[3]) % 100)


def importer_orca(orca, sortie, conv, journal):
    lib = None
    marques = [m for m in marques_de(orca) if m not in MARQUES_ECARTEES]
    # 1) copie + conversion fichier par fichier (la bibliothèque d'abord : les marques en héritent)
    for nom in [BIBLIOTHEQUE] + [m for m in marques if m != BIBLIOTHEQUE]:
        shutil.copytree(os.path.join(orca, nom), os.path.join(sortie, nom))
        index = lire(os.path.join(orca, nom + '.json'))
        index['version'] = version_orio(index.get('version', '02.00.00.00'))
        ecrire(os.path.join(sortie, nom + '.json'), index)
        m = Marque(sortie, nom)
        for liste in LISTES[1:]:
            for e in m.index.get(liste, []):
                j = m.fichiers[(liste, e['name'])]
                conv.convertir_fichier(j, nom, e['sub_path'])
        if nom == BIBLIOTHEQUE:
            lib = m
        # 2) filaments qui héritent de la bibliothèque : aplatis (Orio résout l'héritage marque par marque)
        else:
            propres = {e['name'] for e in m.index.get('filament_list', [])}
            for e in m.index.get('filament_list', []):
                j = m.fichiers[('filament_list', e['name'])]
                parent = j.get('inherits')
                if parent and parent not in propres and ('filament_list', parent) in lib.fichiers:
                    fusion = lib.resoudre('filament_list', parent, meta=True)
                    fid = fusion.pop('filament_id', None)
                    fusion.update({k: v for k, v in j.items() if k != 'inherits'})
                    if fid and not j.get('filament_id'):
                        fusion['filament_id'] = fid
                    m.fichiers[('filament_list', e['name'])] = fusion
                    journal['filaments aplatis'][nom] += 1
        # 3) imprimantes non gérées : retirées, ainsi que les buses / modèles qui n'ont plus d'imprimante
        retirees = {}
        for e in m.index.get('machine_list', []):
            j = m.fichiers[('machine_list', e['name'])]
            if j.get('instantiation') != 'true':
                continue
            cfg = m.resoudre('machine_list', e['name'])
            pb = conv.gcode_inconnu(cfg)
            nd = cfg.get('nozzle_diameter', [])
            if isinstance(nd, list) and len(nd) > 1 and not conv.garder_multi \
                    and str(cfg.get('single_extruder_multi_material', '0')) in ('0', "['0']"):
                pb.add('plusieurs extrudeurs')
            if pb:
                retirees[e['name']] = sorted(pb)
            else:   # l'assistant lit ces champs dans le fichier lui-même, sans suivre l'héritage
                for k in ('printer_model', 'printer_variant', 'nozzle_diameter'):
                    if k not in j and k in cfg:
                        j[k] = cfg[k]
                # vignettes : sans indication, celles d'OrcaSlicer (Bambu Studio n'en met qu'une de 50×50)
                if 'thumbnail_size' not in cfg:
                    j['thumbnail_size'] = ['48x48', '300x300']
                # rétraction par le firmware : pas d'essuyage (Orio refuse les deux ensemble, validate())
                if str(cfg.get('use_firmware_retraction', '0')) in ('1', 'true') and '1' in str(cfg.get('wipe', '0')):
                    n_ext = len(cfg['nozzle_diameter']) if isinstance(cfg.get('nozzle_diameter'), list) else 1
                    j['wipe'] = ['0'] * n_ext
                    journal['essuyage coupé (rétraction firmware)'][(nom, e['name'])] += 1
                if zone_en_cadre(cfg.get('bed_exclude_area', [])):
                    j['bed_exclude_area'] = ['0x0']
                    journal['zones exclues en cadre retirées'][(nom, e['name'])] += 1
                # G-code binaire (.bgcode) des Prusa à firmware Buddy, comme PrusaSlicer : modèle lu dans le contrôle
                # M862.3 P "…" du G-code de démarrage (les MK3S / MK2.5 ne le lisent pas)
                modele = re.search(r'M862\.3 P "([^"]+)"', str(cfg.get('machine_start_gcode', '')))
                if modele and modele.group(1) in MODELES_BGCODE:
                    j['binary_gcode'] = '1'
                    journal['G-code binaire activé'][(nom, e['name'])] += 1
        parents = {m.fichiers[('machine_list', e['name'])].get('inherits') for e in m.index.get('machine_list', [])
                   if e['name'] not in retirees}
        for n, pb in list(retirees.items()):
            journal['imprimantes retirées'][(nom, n)] = pb
            if n in parents:   # base d'imprimantes gardées : reste, sans être proposée
                m.fichiers[('machine_list', n)]['instantiation'] = 'false'
                del retirees[n]
        m.index['machine_list'] = [e for e in m.index.get('machine_list', []) if e['name'] not in retirees]
        variantes = collections.defaultdict(list)
        for e in m.index['machine_list']:
            j = m.fichiers[('machine_list', e['name'])]
            if j.get('instantiation') == 'true':
                cfg = m.resoudre('machine_list', e['name'])
                variantes[cfg.get('printer_model')].append(str(cfg.get('printer_variant')))
        modeles = []
        for e in m.index.get('machine_model_list', []):
            j = m.fichiers[('machine_model_list', e['name'])]
            garde = [v for v in str(j.get('nozzle_diameter', '')).split(';') if v.strip() in variantes.get(e['name'], [])]
            if not garde:
                journal['modèles retirés'][nom].append(e['name'])
                continue
            j['nozzle_diameter'] = ';'.join(v.strip() for v in garde)
            modeles.append(e)
        m.index['machine_model_list'] = modeles
        # filaments à G-code illisible : retirés
        fil_ret = set()
        for e in m.index.get('filament_list', []):
            j = m.fichiers[('filament_list', e['name'])]
            if j.get('instantiation') == 'true':
                pb = conv.gcode_inconnu(m.resoudre('filament_list', e['name'], lib if nom != BIBLIOTHEQUE else None))
                if pb:
                    fil_ret.add(e['name'])
                    journal['filaments retirés'][(nom, e['name'])] = sorted(pb)
        m.index['filament_list'] = [e for e in m.index.get('filament_list', []) if e['name'] not in fil_ret]
        ajuster_nombres(m, lib if nom != BIBLIOTHEQUE else None, conv, journal)
        if not m.index.get('machine_model_list') and nom != BIBLIOTHEQUE:
            journal['marques retirées'][nom] = 'plus aucune imprimante gérée'
            shutil.rmtree(os.path.join(sortie, nom))
            continue
        ecrire(os.path.join(sortie, nom + '.json'), m.index)
        for (liste, n), j in m.fichiers.items():
            if any(e['name'] == n for e in m.index.get(liste, [])):
                ecrire(m.chemin(liste, n), j)


# --- vérification : ce que ferait Orio au chargement -----------------------------------------------------
def verifier(racine, defs):
    erreurs, avert = [], []
    noms = collections.defaultdict(list)   # (type, nom instancié) -> marques
    modeles = collections.defaultdict(list)
    for nom in marques_de(racine):
        try:
            m = Marque(racine, nom)
        except Exception as ex:
            erreurs.append('%s : lecture impossible (%s)' % (nom, ex))
            continue
        v = str(m.index.get('version', ''))
        if not re.fullmatch(r'\d{1,2}\.\d{1,2}(\.\d{1,2}){0,2}', v):
            erreurs.append('%s : version « %s » illisible' % (nom, v))
        variantes = {}
        for e in m.index.get('machine_model_list', []):
            j = m.fichiers[('machine_model_list', e['name'])]
            for k in CHAMPS_MODELE:
                if k in j and not isinstance(j[k], str):
                    erreurs.append('%s / modèle %s : champ %s non textuel' % (nom, e['name'], k))
            variantes[e['name']] = {x.strip() for x in str(j.get('nozzle_diameter', '')).split(';')}
            modeles[e['name']].append(nom)
            if not os.path.exists(os.path.join(racine, nom, e['name'] + '_cover.png')):
                avert.append('%s / %s : pas d\'image de couverture' % (nom, e['name']))
        for liste in LISTES[1:]:
            vus, fil_id = set(), {}
            for e in m.index.get(liste, []):
                n = e['name']
                j = m.fichiers[(liste, n)]
                ou = '%s / %s' % (nom, n)
                if j.get('name') != n:
                    erreurs.append('%s : nom du fichier « %s » différent de l\'index' % (ou, j.get('name')))
                inst = j.get('instantiation')
                if not isinstance(inst, str) or inst not in ('true', 'false'):
                    erreurs.append('%s : « instantiation » absent ou non textuel' % ou)
                par = j.get('inherits')
                if par and par not in vus:
                    erreurs.append('%s : parent « %s » introuvable (ou listé après)' % (ou, par))
                vus.add(n)
                if liste == 'filament_list':
                    fid = j.get('filament_id') or fil_id.get(par, '')
                    if fid:
                        fil_id[n] = fid
                    elif inst == 'true':
                        erreurs.append('%s : pas de filament_id' % ou)
                if inst != 'true':
                    continue
                noms[(liste, n)].append(nom)
                try:
                    cfg = m.resoudre(liste, n)
                except Exception as ex:
                    erreurs.append('%s : %s' % (ou, ex))
                    continue
                if liste == 'machine_list':
                    pm, pv = cfg.get('printer_model', ''), str(cfg.get('printer_variant', ''))
                    if pm not in variantes:
                        erreurs.append('%s : modèle « %s » absent de la marque' % (ou, pm))
                    elif pv not in variantes[pm]:
                        erreurs.append('%s : buse « %s » absente du modèle %s' % (ou, pv, pm))
                    for k in ('printer_model', 'nozzle_diameter'):
                        if k not in j:
                            erreurs.append('%s : %s absent du fichier (assistant)' % (ou, k))
    for (liste, n), ms in noms.items():
        if len(ms) > 1:
            erreurs.append('profil « %s » présent dans plusieurs marques : %s' % (n, ', '.join(ms)))
    for n, ms in modeles.items():
        if len(ms) > 1:
            erreurs.append('modèle « %s » présent dans plusieurs marques : %s' % (n, ', '.join(ms)))
    return erreurs, avert


def bilan_texte(racine, journal, erreurs, avert):
    lignes = ['BILAN DU CATALOGUE — resources/profiles', '']
    tot = collections.Counter()
    for nom in marques_de(racine):
        m = Marque(racine, nom)
        c = {l: sum(1 for e in m.index.get(l, []) if m.fichiers[(l, e['name'])].get('instantiation', 'true') == 'true')
             for l in LISTES[1:]}
        c['machine_model_list'] = len(m.index.get('machine_model_list', []))
        for k, v in c.items():
            tot[k] += v
        lignes.append('  %-22s %-12s %4d modèles %5d imprimantes %5d filaments %5d réglages' % (
            nom, m.index.get('version'), c['machine_model_list'], c['machine_list'], c['filament_list'], c['process_list']))
    lignes.append('  %-35s %4d modèles %5d imprimantes %5d filaments %5d réglages' % (
        'TOTAL (%d marques)' % len(marques_de(racine)), tot['machine_model_list'], tot['machine_list'],
        tot['filament_list'], tot['process_list']))
    lignes += ['', 'MARQUES ÉCARTÉES'] + ['  %s : %s' % kv for kv in MARQUES_ECARTEES.items()]
    lignes += ['  %s : %s' % kv for kv in journal['marques retirées'].items()]
    lignes += ['', 'IMPRIMANTES RETIRÉES (G-code ou matériel non gérés)']
    lignes += ['  %s / %s : %s' % (k[0], k[1], ', '.join(v)) for k, v in sorted(journal['imprimantes retirées'].items())]
    lignes += ['', 'MODÈLES RETIRÉS (plus aucune imprimante)']
    lignes += ['  %s : %s' % (k, ', '.join(v)) for k, v in sorted(journal['modèles retirés'].items())]
    lignes += ['', 'FILAMENTS RETIRÉS']
    lignes += ['  %s / %s : %s' % (k[0], k[1], ', '.join(v)) for k, v in sorted(journal['filaments retirés'].items())]
    lignes += ['', 'FILAMENTS APLATIS (héritaient de la bibliothèque)']
    lignes += ['  %s : %d' % kv for kv in sorted(journal['filaments aplatis'].items())]
    lignes += ['', 'POURCENTAGES CONVERTIS (largeurs en mm d\'après la buse, surplombs en mm/s)']
    lignes += ['  %s : %d' % kv for kv in sorted(journal['pourcentages convertis'].items())]
    lignes += ['  sans buse connue (largeur 0 = automatique) : %s / %s' % k for k in sorted(journal['pourcentages sans buse'])]
    lignes += ['', 'VALEURS RAMENÉES DANS LES BORNES D'ORIO']
    lignes += ['  %s : %s -> %s (%d)' % (k[0], k[1], k[2], n) for k, n in sorted(journal['valeurs ramenées'].items())]
    lignes += ['', 'ESSUYAGE COUPÉ (rétraction par le firmware)']
    lignes += ['  %s / %s' % k for k in sorted(journal['essuyage coupé (rétraction firmware)'])]
    lignes += ['', 'ZONES EXCLUES EN CADRE RETIRÉES (lues comme tout le plateau par Orio)']
    lignes += ['  %s / %s' % k for k in sorted(journal['zones exclues en cadre retirées'])]
    lignes += ['', 'VALEURS RETIRÉES (inconnues de Orio, valeur par défaut appliquée)']
    lignes += ['  %s = %s : %d' % (k[0], k[1], n) for k, n in sorted(journal['valeurs retirées'].items())]
    lignes += ['', 'ERREURS (%d)' % len(erreurs)] + ['  ' + e for e in erreurs]
    lignes += ['', 'AVERTISSEMENTS (%d)' % len(avert)] + ['  ' + a for a in avert]
    return '\n'.join(lignes) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bambu', help='resources/profiles de Bambu Studio')
    ap.add_argument('--orca', help='resources/profiles d\'OrcaSlicer')
    ap.add_argument('--verifier', action='store_true', help='vérifier seulement resources/profiles')
    ap.add_argument('--bilan', default=None, help='fichier où écrire le bilan')
    ap.add_argument('--sortie', default=PROFILS, help='dossier à réécrire (essais) ; par défaut resources/profiles')
    ap.add_argument('--garder-multi', action='store_true', help='garder les imprimantes à plusieurs extrudeurs (essais)')
    a = ap.parse_args()
    sortie = a.sortie
    defs, variables, bornes = lire_definitions()
    journal = collections.defaultdict(lambda: collections.defaultdict(list))
    for k in ('valeurs retirées', 'filaments aplatis', 'pourcentages convertis', 'pourcentages sans buse', 'valeurs ramenées',
              'zones exclues en cadre retirées', 'essuyage coupé (rétraction firmware)', 'G-code binaire activé'):
        journal[k] = collections.Counter()
    journal['marques retirées'] = {}
    if not a.verifier:
        if not (a.bambu and a.orca):
            ap.error('--bambu et --orca sont nécessaires')
        os.makedirs(sortie, exist_ok=True)
        for e in os.listdir(sortie):
            p = os.path.join(sortie, e)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        for e in [BBL + '.json', BBL, 'blacklist.json']:
            s = os.path.join(a.bambu, e)
            shutil.copytree(s, os.path.join(sortie, e)) if os.path.isdir(s) else shutil.copy2(s, sortie)
        conv = Convertisseur(defs, variables, journal)
        conv.garder_multi = a.garder_multi
        conv.bornes = bornes
        importer_orca(a.orca, sortie, conv, journal)
    erreurs, avert = verifier(sortie, defs)
    texte = bilan_texte(sortie, journal, erreurs, avert)
    if a.bilan:
        open(a.bilan, 'w', encoding='utf-8').write(texte)
    print(texte if len(texte) < 6000 else texte[:3000] + '\n[…] voir ' + str(a.bilan))
    return 1 if erreurs else 0


if __name__ == '__main__':
    sys.exit(main())
