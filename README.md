# Makar — jeu de pirates (Unreal Engine 5)

Base jouable d'un jeu de pirates. Elle comprend :
- un **océan FFT maison** : spectre JONSWAP, vagues pointues (« choppy »), écume, 3 couches de vagues ;
- un navire qui flotte dessus avec une flottaison d'Archimède ;
- la navigation à la voile ;
- des bordées de canons ;
- un navire ennemi piloté par une IA.

Tout est en C++. Aucun plugin payant n'est nécessaire.

## Mise en route

1. Installer **Unreal Engine 5.8** (Epic Games Launcher) et **Visual Studio 2022** avec la charge de travail
   « Développement de jeux en C++ ».
   Pour une autre version d'UE 5 : clic droit sur `Makar.uproject` → *Switch Unreal Engine version*.
2. Double-cliquer sur `Makar.uproject` et accepter la compilation des modules.
3. **Générer le matériau de l'océan** (une seule fois) :
   *Tools → Execute Python Script…* → choisir `Scripts/create_ocean_material.py`.
   Cela crée `Content/Ocean/M_Ocean`.
4. Créer le niveau :
   - *File → New Level → Basic*, puis supprimer le sol `Floor`.
   - *Place Actors → All Classes* → glisser un **Ocean Actor** à Z = 0. Son Z définit le niveau de la mer.
     Les vagues bougent déjà dans l'éditeur.
   - Placer le **Player Start** à 2–3 m au-dessus de l'eau.
   - Glisser un ou plusieurs **Enemy Ship** sur l'eau, à quelques centaines de mètres.
   - Enregistrer dans `Content/Maps/Ocean`, puis *Project Settings → Maps & Modes* →
     définir cette carte comme *Editor Startup Map* et *Game Default Map*.
5. *Play*.

### Ambiance « mer du Nord »

Une grande partie du réalisme vient de la lumière et de la caméra, pas seulement de l'eau :

- **Exponential Height Fog** : activer *Volumetric Fog*, avec une densité élevée et une couleur gris-bleu.
- **Directional Light** basse et faible ; **Volumetric Cloud** avec une couverture élevée, pour un ciel couvert.
- **Post Process Volume** (*Infinite Extent*) :
  - profondeur de champ (*Focal Distance* sur le navire, *Aperture* basse) ;
  - *Saturation* réduite et *Temperature* froide ;
  - *Film Grain* léger.

## Commandes

| Action | Touches |
| --- | --- |
| Hisser / affaler les voiles (4 crans) | `Z` / `S` (ou `W` / `S`, flèches haut / bas) |
| Barre à bâbord / tribord | `Q` / `D` (ou `A` / `D`, flèches gauche / droite) |
| Caméra | Souris |
| Bordée bâbord (gauche) | Clic gauche |
| Bordée tribord (droite) | Clic droit |

## Architecture de l'océan

```
FOceanSimulation (tâche de fond, ParallelFor)
  spectre JONSWAP + étalement de Hasselmann → h(k, t) → FFT inverse ×4 par couche
  ├─► textures (déplacement + dérivées, avec mips) ─► M_Ocean (Shaders/Ocean.ush)
  └─► copie CPU ─► AOceanActor::GetWaterHeightAt ─► UOceanSubsystem
                                                      ├─ UOceanBuoyancyComponent (navires)
                                                      └─ (à venir) nage, caméra sous l'eau
```

L'affichage et la physique lisent **les mêmes données**. Ce qui flotte suit donc exactement les vagues visibles.

| Fichier | Rôle |
| --- | --- |
| `Source/Makar/Public/Ocean/OceanSettings.h` | État de la mer : vent, fetch, houle, écume… |
| `Source/Makar/Public/Ocean/OceanSimulation.h` | Simulation FFT (Tessendorf), 3 couches |
| `Source/Makar/Public/Ocean/OceanActor.h` | Maillage infini centré sur la caméra, textures, requêtes de hauteur |
| `Source/Makar/Public/Ocean/OceanSubsystem.h` | Point d'entrée unique pour savoir où est l'eau |
| `Source/Makar/Public/Ocean/OceanBuoyancyComponent.h` | Flottaison par flotteurs sphériques |
| `Shaders/Ocean.ush` | Déplacement des sommets, normales, écume |
| `Scripts/create_ocean_material.py` | Génère le matériau `M_Ocean` |
| `Source/Makar/Public/PirateShip.h` | Navire : voiles, gouvernail, canons, points de coque |
| `Source/Makar/Public/EnemyShip.h` | IA : poursuite, mise en travers, tir |

### Réglages (Ocean Actor → Details)

| Paramètre | Effet |
| --- | --- |
| `WindSpeed` + `Fetch` | Taille des vagues. 10 m/s sur 100 km ≈ 2 m de hauteur significative ; 20 m/s sur 300 km ≈ 5 m |
| `Swell` | Houle longue et régulière |
| `Choppiness` | Crêtes plus pointues (au-delà de ~1,2, les crêtes se replient) |
| `FoamThreshold` / `FoamIntensity` / `FoamDecay` | Quantité d'écume et durée avant qu'elle disparaisse |
| `Resolution` | 128 coûte peu ; 256 donne plus de détail mais coûte environ 4 fois plus en CPU |

Couleur, rugosité et diffusion se règlent dans une *Material Instance* de `M_Ocean`.

## Feuille de route

1. Ambiance de test et réglage du rendu (couleur, écume, reflets).
2. Personnage : marche, **nage**, vue sous l'eau, marche sur le pont d'un navire en mouvement.
3. Vent et allures de navigation.
4. Rivages (vagues qui déferlent), sillage des navires.
5. Simulation FFT sur GPU (compute shader) si le CPU devient limitant.

## Références

- J. Tessendorf, *Simulating Ocean Water* (2001) : surface par FFT, déplacement horizontal.
- C. J. Horvath, *Empirical Directional Wave Spectra for Computer Graphics* (2015) : JONSWAP, étalement directionnel.
