# 3DS Link v1.5 — Smart Pairing

## Association iPhone
- QR mémorisé sur l'iPhone ;
- nom de console personnalisable ;
- test automatique de la liaison directe ;
- latence locale affichée ;
- dernier contact mémorisé ;
- mode de connexion mémorisé.

## Liaison PWA → 3DS
- réponses CORS ajoutées côté 3DS ;
- support des préflights HTTP OPTIONS ;
- header X-3DS-Link-Pin autorisé ;
- support Access-Control-Allow-Private-Network ;
- si iOS bloque quand même HTTPS → HTTP, bascule automatique vers le portail local.

## Portail local
- reçoit le PIN depuis la PWA via le fragment URL ;
- enregistre le PIN localement ;
- se déverrouille automatiquement ;
- peut ouvrir directement Fichiers, Clavier, Remote, Camera ou Infos.

## Camera
- mode direct PWA conservé lorsque Safari l'autorise ;
- sinon Camera Link s'ouvre automatiquement dans le portail local ;
- plein écran conservé en mode PWA direct.

Le moteur caméra v0.8 reste inchangé.
