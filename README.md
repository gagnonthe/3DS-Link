# 3DS Link

3DS Link est un homebrew Nintendo 3DS conçu pour créer un pont local entre une 3DS et un iPhone.

## v0.1 — Connexion de base

Cette première version sert à valider la fondation réseau :

- serveur HTTP local directement sur la 3DS ;
- adresse IP affichée sur l'écran supérieur ;
- interface Citro2D sur les deux écrans ;
- page mobile dédiée accessible depuis Safari ;
- détection d'une connexion iPhone ;
- compteur de requêtes ;
- relance du serveur avec `A`.

## Test

1. Mets l'iPhone et la 3DS sur le même réseau Wi‑Fi.
2. Lance `3DS-Link.3dsx` depuis Homebrew Launcher.
3. L'application affiche une adresse du type `http://192.168.1.25:8080`.
4. Tape exactement cette adresse dans Safari.
5. La page **3DS Link** doit apparaître et la 3DS doit afficher `iPhone detecte`.

## Commandes 3DS

- `A` : relancer le serveur réseau
- `START` : quitter

## Suite prévue

- transfert de fichiers iPhone ↔ SD ;
- envoi de photos ;
- clavier iPhone → 3DS ;
- télécommande ;
- WebSocket pour les échanges temps réel ;
- découverte automatique de la 3DS ;
- interface tactile plus avancée ;
- version CIA avec icône et bannière dédiées.

## Base technique

- devkitARM / libctru
- Citro2D / Citro3D
- sockets BSD
- HTTP local
