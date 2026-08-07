# 3DS Link

3DS Link crée un pont local entre une Nintendo 3DS et un iPhone, directement par Wi‑Fi.

## v0.4

La connexion de base de la v0.1 est maintenant transformée en outil réellement utilisable.

### Fonctions

- serveur HTTP local sur la 3DS ;
- interface Safari adaptée à l’iPhone ;
- code PIN à 4 chiffres affiché sur la 3DS ;
- transfert de fichiers **iPhone → 3DS** ;
- téléchargement de fichiers **3DS → iPhone** ;
- suppression des fichiers depuis Safari ;
- stockage sécurisé dans `sdmc:/3ds/3DS-Link/inbox/` ;
- transfert en streaming : les gros fichiers ne sont pas chargés entièrement en RAM ;
- limite actuelle d’upload : 64 Mo ;
- clavier iPhone → écran de la 3DS ;
- télécommande expérimentale de l’application 3DS Link ;
- QR code de connexion généré directement par la 3DS ;
- journal d’activité sur l’écran inférieur.


### Camera Link (v0.4)

- mode appareil photo accessible avec `Y` ;
- capture avec la camera exterieure de la 3DS ;
- gros declencheur tactile ou bouton `A` ;
- prises multiples pendant la meme session ;
- photos BMP 400×240 enregistrees dans `sdmc:/3ds/3DS-Link/camera/` ;
- nouvel onglet **Camera** sur l'iPhone ;
- une nouvelle photo est recuperee automatiquement par Safari et affichee dans la pellicule ;
- declenchement possible depuis l'iPhone ;
- telechargement et suppression de chaque photo.

Cette premiere etape privilegie la fiabilite de la capture et du transfert. Le retour video continu dans le viseur sera ajoute apres validation sur la vraie console, car il demande une boucle camera/GPU differente du rendu Citro2D actuel.

## Utilisation

1. Mets l’iPhone et la 3DS sur le même Wi‑Fi.
2. Lance `3DS-Link.3dsx`.
3. Scanne le QR code de l’écran supérieur avec l’appareil photo de l’iPhone (ou saisis l’adresse affichée).
4. Safari ouvre directement 3DS Link ; entre ensuite le code PIN à 4 chiffres affiché sur la console.
5. Utilise les onglets Fichiers, Clavier ou Remote.

## Commandes 3DS

- `A` : relancer le serveur (accueil) / prendre une photo (mode Camera)
- `Y` : ouvrir/fermer Camera Link
- `B` : revenir à l’accueil depuis Camera Link
- `X` : générer un nouveau PIN
- `START` : quitter

## Dossier de réception

`sdmc:/3ds/3DS-Link/inbox/`

## Prochaines étapes

- serveur réseau dans un thread séparé pour garder l’interface fluide pendant les transferts ;
- explorateur SD avec dossiers ;
- déplacement/copie de fichiers ;
- aperçu d’images ;
- WebSocket pour le temps réel ;
- presse-papiers ;
- favoris et raccourcis ;
- version CIA avec icône et bannière dédiées.


## QR code

La v0.3 utilise la bibliothèque **QR Code generator** de Project Nayuki (licence MIT) pour produire un vrai QR code standard et scannable à partir de l’adresse locale de la 3DS.

## Automatisation GitHub

Le numéro de version est stocké dans `VERSION`. Le workflow de compilation lit ce fichier pour nommer automatiquement l’artifact. Le workflow de mise à jour peut ensuite déclencher `build.yml` via `workflow_dispatch`, ce qui évite de lancer manuellement deux workflows.
