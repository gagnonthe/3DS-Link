# 3DS Link

## v0.9 — Instant Sync

La v0.9 conserve le moteur caméra v0.8 intact et améliore uniquement la synchronisation
entre la 3DS et l'iPhone.

### Photo de la session

- le compteur n'augmente qu'après une sauvegarde BMP réussie ;
- l'écran inférieur est redessiné immédiatement après la prise ;
- le nom de la dernière photo apparaît sans devoir quitter puis rouvrir Camera Link ;
- le viseur supérieur continue d'utiliser le framebuffer natif v0.8.

### Latence iPhone ↔ 3DS

- Safari actualise la pellicule toutes les ~550 ms au lieu de 2,2 s ;
- après un déclenchement iPhone, la recherche de la nouvelle photo passe de 650 ms à 250 ms ;
- le serveur traite jusqu'à plusieurs requêtes en attente par frame sur l'accueil ;
- les sockets ont désormais un timeout court pour qu'une connexion lente ne fige pas l'application ;
- pendant Camera Link, une requête légère peut être traitée régulièrement sans arrêter le viseur ;
- les gros transferts de fichiers sont temporairement refusés pendant le viseur afin de préserver sa fluidité.

### Ce qui doit fonctionner pendant Camera Link

- consultation de l'état caméra ;
- demande de capture depuis l'iPhone ;
- clavier distant ;
- commandes Remote ;
- rafraîchissement de la pellicule.

Les uploads/downloads lourds restent disponibles dès que l'utilisateur quitte Camera Link.

### Important

Le flux vidéo direct vers Safari reste volontairement désactivé dans cette version.
Il sera réintroduit ensuite sur une architecture séparée, sans toucher à la boucle caméra
v0.8 désormais validée sur la vraie console.
