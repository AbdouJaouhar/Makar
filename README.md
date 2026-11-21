# Makar — jeu de pirates (Unreal Engine 5)

Base jouable d'un jeu de pirates : un navire qui flotte sur l'océan du plugin **Water**
d'Unreal (vagues de Gerstner, écume, réflexions), navigation à la voile, bordées de canons
et un navire ennemi piloté par une IA.

Tout le gameplay est en C++ ; aucun asset binaire n'est nécessaire pour jouer
(les navires utilisent des formes de base du moteur en attendant de vrais modèles).

## Mise en route

1. Installer **Unreal Engine 5.6** (Epic Games Launcher) et **Visual Studio 2022** avec la
   charge de travail « Développement de jeux en C++ ».
   Autre version d'UE 5 : clic droit sur `Makar.uproject` → *Switch Unreal Engine version*.
2. Double-cliquer sur `Makar.uproject` et accepter la compilation des modules.
3. Créer le niveau (une seule fois, ~5 minutes) :
   - *File → New Level → Basic* (ciel et lumière déjà en place), supprimer le sol `Floor`.
   - Panneau *Place Actors* → chercher **Water Body Ocean** et le glisser dans la scène
     (une *Water Zone* est ajoutée automatiquement).
   - Placer le **Player Start** au-dessus de l'eau, à 2–3 m de la surface.
   - Glisser un ou plusieurs **Enemy Ship** (*Place Actors → All Classes*) sur l'eau, à quelques
     centaines de mètres.
   - Enregistrer dans `Content/Maps/Ocean`, puis *Project Settings → Maps & Modes* →
     définir cette carte comme *Editor Startup Map* et *Game Default Map*.
4. *Play*.

## Commandes

| Action | Touches |
| --- | --- |
| Hisser / affaler les voiles (4 crans) | `Z` / `S` (ou `W` / `S`, flèches haut / bas) |
| Barre à bâbord / tribord | `Q` / `D` (ou `A` / `D`, flèches gauche / droite) |
| Caméra | Souris |
| Bordée bâbord (gauche) | Clic gauche |
| Bordée tribord (droite) | Clic droit |

## Code

| Fichier | Rôle |
| --- | --- |
| `Source/Makar/Public/PirateShip.h` | Navire : flottabilité, voiles, gouvernail, canons, points de coque |
| `Source/Makar/Public/EnemyShip.h` | IA : poursuite, mise en travers, tir |
| `Source/Makar/Public/Cannonball.h` | Boulet balistique |
| `Source/Makar/Public/MakarGameMode.h` | Fait du navire le pion du joueur |

## Réglages

Tous les paramètres sont modifiables dans l'éditeur (catégorie *Ship*) :

- **Le navire s'enfonce trop ou flotte trop haut** : ajuster la masse de `Hull`
  (3000 kg par défaut) ou `Buoyancy → Buoyancy Data → Buoyancy Coefficient`.
- **Vitesse maximale** = `SailAcceleration / ForwardDrag` (1250 cm/s ≈ 24 nœuds par défaut).
- **Vagues** : sélectionner le *Water Body Ocean* → *Wave Source* (hauteur, longueur,
  direction des vagues de Gerstner).

## Suite possible

- Créer un Blueprint enfant de `PirateShip` pour y mettre un vrai modèle de navire,
  des voiles animées, du son et des effets (l'événement `OnSunk` est prévu pour le naufrage).
- Vent (allures, direction), îles (Landscape + *Water Body Island*), abordage, butin, HUD.
