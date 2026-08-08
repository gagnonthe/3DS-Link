# 3DS Link

## v1.0 — Responsive Link

Cette version conserve le moteur caméra natif validé en v0.8 et refait la
synchronisation iPhone autour de lui.

### Corrections principales

- flux caméra iPhone réactivé ;
- flux réduit à 160×96 (~46 Ko par image) pour protéger le framerate de la 3DS ;
- une requête réseau peut être traitée après chaque frame caméra ;
- côté Safari, une seule boucle réseau pilote le flux, la pellicule et l'attente
  d'une capture : plus de requêtes concurrentes qui s'empilent ;
- la liste des photos de session est conservée en RAM au lieu de rescanner la SD
  toutes les 550 ms ;
- après une capture, une miniature exacte de la frame est gardée en RAM et envoyée
  immédiatement à l'iPhone ;
- le bouton Voir récupère ensuite le BMP complet depuis la SD ;
- `/camera/file` et la suppression de photo fonctionnent de nouveau pendant
  Camera Link ;
- uploads/downloads généraux restent bloqués pendant le viseur pour préserver sa
  fluidité.

### Objectif

- déclenchement iPhone → 3DS : quelques dizaines à quelques centaines de ms ;
- retour miniature après photo : immédiat après l'écriture SD ;
- flux iPhone : environ 4–5 images/s selon le réseau local ;
- viseur 3DS : moteur natif v0.8 inchangé.
