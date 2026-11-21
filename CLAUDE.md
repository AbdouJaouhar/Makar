# Makar — conventions

## Git
- Chaque nouvelle fonctionnalité, amélioration ou refactor se fait sur **une nouvelle branche**, créée depuis
  la branche de base à jour (`feature/<nom>`, `improve/<nom>`, `refactor/<nom>`). Jamais directement sur la base.

## Projet
- Unreal Engine 5, gameplay en C++ (`Source/Makar`), shaders dans `Shaders/` (chemin virtuel `/Makar`).
- Toute requête « où est l'eau ? » passe par `UOceanSubsystem` : rendu et physique doivent rester cohérents.
- Les assets `.uasset` sont générés par des scripts Python d'éditeur (`Scripts/`) quand c'est possible.
