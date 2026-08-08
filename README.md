# 3DS Link

## v0.5 — Live Camera

Cette version transforme Camera Link en véritable viseur connecté.

### Nouveautés

- caméra extérieure maintenue active tant que le mode Camera est ouvert ;
- aperçu vidéo réel sur l'écran supérieur de la 3DS ;
- flux quasi temps réel vers Safari sur l'iPhone ;
- Safari récupère une nouvelle image environ toutes les 420 ms ;
- prise de photo depuis la 3DS ou depuis l'iPhone ;
- plusieurs photos successives sans réinitialiser la caméra ;
- couleurs RGB565 corrigées avec conversion 5/6 bits complète vers 8 bits ;
- l'exposition et la balance des blancs ont le temps de se stabiliser avant une photo ;
- photos BMP 400×240 enregistrées dans `sdmc:/3ds/3DS-Link/camera/` ;
- pellicule, téléchargement et suppression toujours disponibles.

### Utilisation

1. Lance 3DS Link.
2. Scanne le QR code et entre le PIN.
3. Ouvre l'onglet **Camera** sur l'iPhone, ou appuie sur `Y` sur la 3DS.
4. Attends que l'indicateur passe sur **LIVE**.
5. Appuie sur `A`, le déclencheur tactile, ou **Prendre une photo** sur l'iPhone.

### Remarque technique

Le flux iPhone utilise des instantanés BMP successifs plutôt qu'un encodage H.264. C'est volontaire : la 3DS peut ainsi afficher le viseur et transmettre les images sans ajouter un encodeur vidéo lourd.

### Commandes

- `Y` : ouvrir/fermer Camera Link
- `A` : prendre une photo en mode Camera
- `B` : revenir à l'accueil
- `X` : nouveau PIN sur l'accueil
- `START` : quitter
