# 3DS Link

## v0.8 — Camera core rebuilt from devkitPro reference

La v0.8 reconstruit la boucle vidéo CAMU à partir du fonctionnement de
`devkitPro/3ds-examples/camera/video`.

### Pourquoi la v0.7 pouvait rester bloquée

La v0.7 attendait seulement l'événement de réception d'image. L'exemple officiel
surveille également l'interruption **buffer error** de CAMU et redémarre la
capture lorsqu'elle se produit. Sans ce traitement, une erreur du buffer pouvait
faire enchaîner les timeouts et laisser l'écran sur « Démarrage de la caméra ».

### v0.8

- `CAMU_GetBufferErrorInterruptEvent` utilisé comme dans l'exemple officiel ;
- `CAMU_SetReceiving` réarmé après chaque frame ;
- attente simultanée de l'événement d'erreur et de l'événement de réception ;
- reprise de `CAMU_StartCapture` après une interruption ;
- timeout = resynchronisation, jamais une attente infinie ;
- buffer caméra alloué avec `malloc`, comme l'exemple de référence ;
- caméra extérieure en 400×240 RGB565 à 30 fps ;
- copie directe du RGB565 vers le framebuffer RGB8 du top screen ;
- double buffering du haut activé ;
- écran inférieur figé pendant la caméra pour ne pas perturber la boucle vidéo ;
- serveur/streaming iPhone toujours désactivé en mode Camera pour ce test.

### Test attendu

L'ouverture de Camera Link ne doit plus prendre une minute.
La première image devrait apparaître rapidement, puis suivre le mouvement de la
console continuellement.

Cette version doit être validée sur la vraie 3DS avant de réintroduire le direct iPhone.
