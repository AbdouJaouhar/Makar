# Stratégie de génération procédurale du sable

> **Objectif** : un sable de plage caribéenne **photoréaliste**, du très gros plan (30 cm) à la vue
> d'ensemble (1 km), en **temps réel** dans Unreal Engine 5 (cible : RTX 2070 Super), cohérent avec l'océan
> déjà simulé (vagues de rivage, nappe de reflux, profondeur, écume).

Sommaire : [Principes](#1-principes) · [Couches](#2-les-couches-du-plus-grand-au-plus-petit) ·
[Écarté](#3-ce-qui-est-volontairement-écarté) · [Validation](#4-validation) · [Feuille de route](#5-feuille-de-route) ·
[Sources](#6-sources)

---

## 1. Principes

| # | Principe | Inspiré de |
|---|---|---|
| 1 | **Multi-échelle sans couture** : chaque échelle a sa méthode ; quand ses détails deviennent plus petits qu'un pixel, elle se fond dans ses **statistiques** (albédo moyen, rugosité, densité de scintillement), comme le LEAN mapping de l'océan. Aucune échelle ne disparaît brutalement. | Venu et al. |
| 2 | **Aucun motif répété** : tout est fonction de la position monde — bruit **à gradients** et **grilles de cellules hachées** (générateur pseudo-aléatoire initialisé par l'indice de cellule). Mêmes fonctions sur CPU et GPU. | Venu et al., Thorimbert & Chopard |
| 3 | **La physique place les formes** : on réutilise les champs déjà simulés (profondeur, direction des vagues via le temps de trajet, portée du reflux, vent). Rides, tri des grains et humidité apparaissent là où la nature les mettrait. | Finn et al., Sumner et al. |
| 4 | **Calibré sur des mesures** : formes et tailles de grains (Sand Atlas), angle de talus (Sumner), lois de rides (Finn), morphologie de plage (ICCE), dimension fractale des côtes (Thorimbert & Chopard), et **photos de référence** comme juge final. | toutes |

---

## 2. Les couches, du plus grand au plus petit

### Couche 0 — Fondations

- **Bruit à gradients** (Thorimbert & Chopard) : polynôme bicubique par cellule, contraint en **hauteur et en
  pente** aux coins (configuration « D2M1N3 »). Notre bruit de valeur actuel correspond à leur cas
  *à gradient nul* : les pentes nulles aux nœuds créent des plats alignés sur la grille (le motif « camouflage »
  vu sur les coraux). Des gradients aléatoires aux coins suppriment cet artefact.
- **Grille hachée de particules** (Venu et al.) : une particule par cellule (position, taille, orientation,
  type), double grille décalée d'une demi-cellule pour ne tester que les voisines, rejet probabiliste selon
  une fonction de densité spatiale (« roulette russe »).
- **Côte fractale mesurée** (Thorimbert & Chopard) : dimension fractale du trait de côte par comptage de
  boîtes ; octaves et persistance réglées pour viser **D ≈ 1,15–1,25** (côtes réelles, mesures de Richardson).

### Couche 1 — Morphologie de plage (10 m – 1 km)

Calculée dans le terrain de l'île (`AOceanBathymetry`, CPU), d'après l'ICCE 2022 :

- **Profil** : estran, **berme**, haut de plage, **arrière-plage variée** le long de la côte — dune basse
  végétalisée, terrasse d'accumulation, **lagune côtière** ou falaise.
- **Plages exposées ou abritées par le récif** : abritées → plus plates, sable plus fin ; exposées → plus
  pentues, sable plus grossier. L'exposition se déduit de la houle de rivage.
- **Éventails de débordement** (*overwash*) du sable vers les lagunes ; **barres sableuses** parallèles au
  rivage sous l'eau.
- **Sortie** : le terrain et des **cartes d'attributs** (type et taille de sable, exposition, type
  d'arrière-plage, portée de l'eau) lues par les couches suivantes.

### Couche 2 — Formes du lit (1 cm – 1 m)

| Zone | Forme | Règle | Source |
|---|---|---|---|
| Sable sec | **Rides éoliennes** | Profil **asymétrique** (pente douce au vent, raide sous le vent), λ = 5–15 cm, crêtes ⟂ au vent, défauts de crête (bifurcations) par déformation de la phase ; effacées sur sable mouillé et piétiné | — |
| Sous l'eau | **Rides de vagues** | Crêtes **∥ aux fronts de vagues** (gradient du temps de trajet), λ ≈ 0,65 × diamètre d'orbite au fond (d₀ = H / sinh(kh)) ; **fond plat** dans la zone de déferlement (*sheet flow*) | Finn et al. |
| Sous l'eau | **Tri des grains** | Creux de rides plus sombres et grossiers, crêtes fines et claires (« blindage » grossier en surface) | Finn et al. |
| Zone de reflux | **Sable lissé et glacé** | Arcs de reflux (fines lignes de débris/écume aux limites de montée), petites rigoles de retrait | ICCE, notre simulation |
| Partout à terre | **Traces de pas** | Simulation de colonnes à 1–2 cm : collision → déplacement (compression α = 0,3) → érosion jusqu'à **θ_out = 0,436 rad** (angle de talus) → retombées. Chemins aléatoires plus denses près de l'eau et des accès ; lissés par le vent sur le sec, effacés sous le reflux | Sumner et al. |

**Mise en œuvre** : quelques **tuiles de relief précalculées** au chargement (~8 × 8 m, ~1,5 cm par texel :
hauteur, normale, masque de perturbation), posées sur la grille hachée avec rotation/décalage aléatoires et
**mélangées** pour ne jamais se répéter. Rendu en **parallaxe avec auto-ombrage** près de la caméra, en normales
à moyenne distance, en rugosité filtrée au loin.

### Couche 3 — Grains (0,1 – 5 mm)

Uniquement dans le shader.

- **Grains** (Pixelbuffer) : cellules de Voronoï sur **deux tailles superposées** ; chaque grain a un **dôme**
  (normale), une **occlusion dans les interstices** et une **couleur tirée par grain** (hash de la cellule).
- **Calibration** (Sand Atlas) : rapports d'axes et orientations des grains tirés des descripteurs mesurés ;
  tailles en loi **log-normale (d₅₀, σ_g)** comme chez Finn et al. Trois sables, mélangés selon la couche 1 :

  | Sable | Grains | Couleur |
  |---|---|---|
  | **Oolithique** (Caïques) | ronds, lisses, fins | très blanc |
  | **Biogène** | débris coralliens et coquilliers plats et anguleux, **foraminifères** | crème, points rouge-rose (teinte rosée) |
  | **Terrigène** | anguleux | quelques grains sombres, quartz brillant |

- **Scintillement** : une facette d'orientation aléatoire par grain, stable (liée au hash) ; au soleil, quelques
  grains brillent sans clignoter ; au loin, une densité de scintillement.
- **Passage au loin** : au-delà de 1–2 m, couleur moyenne du mélange + rugosité issue de la variance des normales
  des grains + scintillement moyen.
- **Option très gros plan** (< 0,5 m, Venu et al.) : de vrais grains en **SDF**, tracés par *sphere tracing* dans
  une couche de quelques millimètres.

### Couche 4 — L'eau dans le sable (reliée à notre simulation)

- **Sable mouillé** : plus sombre, plus saturé, plus lisse ; frontière sec/mouillé irrégulière.
- **Pellicule d'eau** juste après le retrait de la nappe : un reflet de ciel qui disparaît à mesure que l'eau
  s'infiltre (temps écoulé depuis le dernier recouvrement, donné par le cycle de reflux).
- **Eau laiteuse dans le ressac** (Finn et al.) : diffusion accrue dans le matériau de l'eau là où les vagues
  cassent, par le sable en suspension.

### Couche 5 — Débris et traces de vie

Grille hachée de Venu et al. ; coquillages et coraux éventuellement générés avec Infinigen.

- **Laisse de sargasses** (très caribéenne), fragments de coquillages, **débris de corail** ;
- **terriers de crabes fantômes** avec leurs projections de sable ;
- feuilles de raisinier bord de mer près de la végétation.

---

## 3. Ce qui est volontairement écarté

| Idée | Pourquoi | Ce qu'on en garde |
|---|---|---|
| Simulation CFD-DEM de Finn et al. | Supercalculateur | Ses **conclusions** : orientation des rides, tri, fond plat, turbidité |
| Géométrie réelle et path tracing d'Infinigen | Offline (3,5 h par paire d'images 1080p) | Au mieux un **générateur d'assets** (coquillages, coraux, plantes) |
| Interaction en direct de Sumner et al. | Pas de personnage | La simulation, pour **précalculer** une plage « vécue » |

---

## 4. Validation

- **Photos de référence** par point de vue : gros plan 0,5 m, debout 1,7 m, 10 m, vue aérienne ; comparaison
  côte à côte, captures par itération dans `Saved/Iterations/NN_nom/`.
- **Mesures objectives** :
  - albédo moyen et histogramme de couleurs comparés aux photos ;
  - longueur d'onde des rides ;
  - **dimension fractale de la côte** ;
  - formes de grains comparées au Sand Atlas ;
  - **absence de répétition** (pics dans la transformée de Fourier des captures) ;
  - stabilité temporelle du scintillement.
- **Budget** : ≈ **2 ms GPU** pour le sable en 1080p ; pas de parallaxe limités ; SDF de grains seulement en très
  gros plan.

---

## 5. Feuille de route

| # | Étape | Sources | Effet attendu |
|---|---|---|---|
| 1 | Bruit à gradients, côte fractale mesurée | Thorimbert & Chopard | Plus d'artefacts de grille, côte naturelle |
| 2 | **Grains, couleurs, scintillement, passage au loin** | Pixelbuffer, Sand Atlas | Le plus gros gain visible de près |
| 3 | Tuiles de relief : rides éoliennes, traces de pas simulées | Sumner et al. | Plage « vécue » à hauteur d'homme |
| 4 | Rides sous l'eau, tri des grains, eau laiteuse | Finn et al. | Fond et ressac crédibles |
| 5 | Sable mouillé, pellicule, arcs de reflux, laisse | Notre simulation, ICCE | Limite eau/sable vivante |
| 6 | Morphologie : berme, dune, lagune, exposition | ICCE 2022 | Plages variées le long de l'île |
| 7 | *(Option)* grains SDF en gros plan, débris Infinigen | Venu et al., Infinigen | Très gros plan |

---

## 6. Sources

1. R. W. Sumner, J. F. O'Brien, J. K. Hodgins, *Animating Sand, Mud, and Snow*, Computer Graphics Forum 18(1),
   1999 — [arXiv:2302.08683](https://arxiv.org/pdf/2302.08683)
2. B. Venu, A. Bosak, J. R. Padrón-Griffe, *Procedural Multiscale Geometry Modeling using Implicit Functions*,
   EGSR 2025 / CGF 44(4) — [arXiv:2504.09553](https://arxiv.org/html/2504.09553v1)
3. A. Raistrick, L. Lipson, Z. Ma et al., *Infinite Photorealistic Worlds using Procedural Generation*
   (Infinigen), CVPR 2023 — [arXiv:2306.09310](https://arxiv.org/html/2306.09310v2)
4. Pixelbuffer, *Procedural Sand Texture* (Blender / Cycles) —
   [pixelbuffer.com](https://pixelbuffer.com/project/procedural-sand-texture/)
5. J. R. Finn, M. Li, S. V. Apte, *Particle based modelling and simulation of natural sand dynamics in the wave
   bottom boundary layer*, J. Fluid Mech. 796, 2016 (CC-BY) —
   [doi:10.1017/jfm.2016.246](https://doi.org/10.1017/jfm.2016.246)
6. Y. Thorimbert, B. Chopard, *Polynomial methods for fast Procedural Terrain Generation*, 2018 —
   [arXiv:1610.03525](https://arxiv.org/pdf/1610.03525)
7. I. Vego, B. Marks, *The Sand Atlas*, 2025 (données CC-BY) — [arXiv:2502.07235](https://arxiv.org/html/2502.07235v1),
   [sand-atlas.scigem.com](https://sand-atlas.scigem.com)
8. J. L. Juanes, C. P. Jack, M. Izquierdo, V. Caballero, P. Morales, *Erosion Process in the Caribbean Sandy
   Beaches*, ICCE 2022, résumé étendu n° 1211.
