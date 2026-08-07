# 3DS Link

3DS Link crée un pont local entre une Nintendo 3DS et un iPhone, directement par Wi‑Fi.

## v0.3

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

## Utilisation

1. Mets l’iPhone et la 3DS sur le même Wi‑Fi.
2. Lance `3DS-Link.3dsx`.
3. Scanne le QR code de l’écran supérieur avec l’appareil photo de l’iPhone (ou saisis l’adresse affichée).
4. Safari ouvre directement 3DS Link ; entre ensuite le code PIN à 4 chiffres affiché sur la console.
5. Utilise les onglets Fichiers, Clavier ou Remote.

## Commandes 3DS

- `A` : relancer le serveur
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
