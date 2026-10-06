# Comparaison

## Conditions

- Control Resonant (DX12), RTX 4070, 2560x1440, DLSS Équilibré : rendu interne 1484x835.
- Benchmark intégré (menu > Benchmark, ou fichier `dlssnr-benchmark.trigger` à côté d'OptiScaler) :
  les modes à la suite sur la même scène, 3 s de stabilisation puis 8 s de mesure chacun.
- FPS : intervalle entre deux appels de l'upscaler, c'est-à-dire les images que le jeu calcule. La
  génération d'images est exclue exprès : elle multiplie le chiffre affiché sans rien coûter ici.
- Coût DLSS 5 : la passe complète sur le GPU (copies, modèle, composition, cache), moyennée.
- Après chaque mode, 10 images consécutives de la sortie de l'upscaler sont relues :
  - scintillement : variation moyenne de luminosité d'une image à la suivante, sans les 5 % de
    pixels qui bougent le plus (objets animés, particules) ;
  - la première est enregistrée en PNG, développée avec la même exposition pour tous les modes.
- Détail ajouté : énergie du laplacien de la luminance (log) sur la partie fixe de la scène,
  rapportée au mode désactivé. Calculé hors jeu sur les PNG.

## Série 1 : réglage par défaut (Quality)

| Mode | FPS | 1 % low | Temps d'image | Coût DLSS 5 | Scintillement | Détail |
|---|---:|---:|---:|---:|---:|---:|
| DLSS 5 désactivé | 62,2 | 28,3 | 16,08 ms | - | 0,30 % | x1,00 |
| DLSS 5 d'origine | 34,4 | 15,1 | 29,04 ms | 13,03 ms | 0,57 % | x1,12 |
| Quality (pre-SR, chaque image) | 46,7 | 23,3 | 21,40 ms | 5,53 ms | 0,40 % | x1,13 |
| Même chose après SR (67 % + agrandissement guidé) | 41,3 | 16,2 | 24,22 ms | 8,16 ms | 0,44 % | x1,14 |

## Série 2 : modèle une image sur deux

| Mode | FPS | 1 % low | Temps d'image | Coût DLSS 5 | Scintillement | Détail |
|---|---:|---:|---:|---:|---:|---:|
| DLSS 5 désactivé | 62,1 | 24,4 | 16,09 ms | - | 0,30 % | x1,00 |
| DLSS 5 d'origine | 34,4 | 15,5 | 29,05 ms | 13,05 ms | 0,43 % | x1,12 |
| Performance (pre-SR, 1 image sur 2) | 55,3 | 24,3 | 18,10 ms | 2,89 ms | 0,47 % | x1,10 |
| Balanced (après SR, 67 %, 1 image sur 2) | 48,1 | 18,5 | 20,79 ms | 4,68 ms | 0,42 % | x1,15 |

Le scintillement du mode d'origine varie d'une série à l'autre (0,43 à 0,57 %) : la scène contient
des débris animés. Les écarts de moins de 0,05 point ne sont pas significatifs.

## Ce que les essais ont appris

- Le pre-SR seul (sans cache) garde tout le détail du modèle : x1,12 à x1,28 selon la vue, autant ou
  plus que le mode d'origine, pour 47 à 53 FPS.
- Avec le cache, le pre-SR perdait du détail (x1,05) : les mises à jour lissées et le stabilisateur
  temporel mélangeaient des réponses du modèle calculées pour des positions de jitter différentes.
  En pre-SR, ces deux filtres s'effacent maintenant (l'accumulation de DLSS SR fait ce travail) :
  x1,09 à x1,15.
- La compensation du jitter dans le cache réduit le scintillement du pre-SR de 0,54 à 0,47 %.

## Face aux autres DLSS 5 (6 octobre 2026)

Mêmes conditions, même scène, chaque outil réglé sur ses options les plus rapides :

- **ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass v0.9.34** : pre-SR, modèle à 100 % puis 75 %, point
  blanc automatique (`WhitePointSource=3`). Avec son réglage par défaut (point blanc manuel), le
  modèle ne produit presque rien dans Control (détail x0,99, effet 0,03 stop).
- **janblade/OptiScaler-F5-DLSSNR-Multipass v0.1.27** : pre-SR avec réutilisation du goulot du
  modèle une image sur deux (`VitEvery=2`), puis après SR avec `DetailReuse` et résolution du modèle
  automatique.
- **Addon RenoDX DLSS5** (build du 19 septembre) : ne fonctionne pas dans Control Resonant. Sans les
  hooks Streamline il ne s'enclenche jamais (0 évaluation), avec `EnableHooks=1` le jeu reste figé au
  démarrage. Les versions récentes ne sont distribuées que sur le Discord RenoDX, non testées ici.

Le coût est celui que chaque outil journalise pour sa passe complète. Comme ces forks remplacent
OptiScaler, notre benchmark ne peut pas les mesurer directement : les FPS sont estimés par
1000 / (16,08 ms + coût), 16,08 ms étant le temps d'image mesuré sans DLSS 5. La formule retombe à
1 % près sur nos mesures réelles (vanilla 34,4 mesuré, 34,4 estimé ; Quality 46,7 et 46,3).

Le scintillement, le détail et l'effet sont mesurés de la même façon pour tous, sur 12 captures
d'écran consécutives (zone fixe de la scène, personnage exclu) : un jeu HDR capturé en SDR, donc les
valeurs absolues diffèrent du tableau du dessus, seuls les écarts comptent.

| Mode | Coût DLSS 5 | FPS estimés | Scintillement | Détail | Effet |
|---|---:|---:|---:|---:|---:|
| DLSS 5 désactivé | - | 62,2 | 0,34 % | x1,00 | - |
| DLSS 5 d'origine (OptiScaler) | 13,03 ms | 34,4 | 0,38 % | x1,14 | 0,12 stop |
| **Nous : Performance** (pre-SR, 1 image sur 2) | **2,89 ms** | **52,7** | 0,40 % | x1,19 | 0,15 stop |
| F5 : après SR + DetailReuse | 3,50 ms | 51,1 | 0,52 % | x1,23 | 0,15 stop |
| ShyVortex : pre-SR 75 % | 3,74 ms | 50,5 | 0,37 % | x1,11 | 0,17 stop |
| F5 : pre-SR, VitEvery 2 | 4,85 ms | 47,8 | 0,41 % | x1,25 | 0,13 stop |
| ShyVortex : pre-SR 100 % | 5,35 ms | 46,7 | 0,45 % | x1,34 | 0,18 stop |
| **Nous : Quality** (pre-SR, chaque image) | 5,53 ms | 46,3 | **0,36 %** | x1,32 | 0,15 stop |

Ce qu'on en retient :

- Notre mode Performance est le moins cher de tous (2,89 ms, 17 % de moins que le suivant), avec
  plus de détail que ShyVortex à 75 % et moins de scintillement que la DetailReuse de F5.
- Notre mode Quality est parmi les plus stables (0,36 %, à égalité avec ShyVortex à 75 % qui a
  nettement moins de détail), avec autant de détail que ShyVortex à 100 % qui scintille plus. Il coûte en revanche 0,7 ms de plus que le pre-SR de F5, qui réutilise une
  partie du calcul interne du modèle d'une image à l'autre : c'est la piste à reprendre.
- Les écarts de scintillement de moins de 0,05 point et d'effet de moins de 0,02 stop sont dans le
  bruit de mesure (une seule scène, une passe par mode).

## Styles du modèle et multi-pass (base OptiScaler du 6 octobre)

Deux séries, chacune avec ses propres références (off et d'origine), réglage Quality :

| Mode | FPS | 1 % low | Coût DLSS 5 | Scintillement | Détail | Effet | Décalage moyen |
|---|---:|---:|---:|---:|---:|---:|---:|
| DLSS 5 d'origine (série 1) | 35,1 | 28,1 | 13,11 ms | 0,43 % | x1,11 | 0,29 stop | -0,11 stop |
| Style Default | 47,4 | 29,9 | 5,54 ms | 0,40 % | x1,12 | 0,29 stop | -0,12 stop |
| Style Natural | 47,6 | 36,1 | 5,63 ms | 0,36 % | x1,19 | 0,28 stop | -0,23 stop |
| Style Cinematic | 47,1 | 32,0 | 5,62 ms | 0,42 % | x0,87 | 0,25 stop | -0,19 stop |
| DLSS 5 d'origine (série 2) | 35,0 | 29,3 | 13,11 ms | 0,52 % | x1,12 | 0,29 stop | -0,12 stop |
| 1 passe | 47,7 | 27,3 | 5,55 ms | 0,38 % | x1,13 | 0,29 stop | -0,13 stop |
| 2 passes | 37,6 | 28,8 | 10,82 ms | 0,47 % | x1,25 | 0,44 stop | -0,21 stop |
| 2 passes, modèle 1 image sur 2 (série 3) | 48,0 | 29,7 | 5,52 ms | 0,63 % | x1,18 | 0,43 stop | - |

- Le style ne change pas le coût. Natural ajoute plus de micro-détail que Default et assombrit
  davantage ; Cinematic lisse. C'est la principale raison pour laquelle un réglage RenoDX en Natural
  ne ressemble pas au rendu par défaut de ce fork.
- Une deuxième passe coûte un passage complet du modèle et renforce nettement l'effet. Avec le cache
  (modèle une image sur deux), deux passes coûtent autant qu'une passe à chaque image.

## Captures

| | |
|---|---|
| ![](benchmark/vanilla.jpg) DLSS 5 d'origine | ![](benchmark/optimise-presr.jpg) Quality (pre-SR) |
| ![](benchmark/off.jpg) DLSS 5 désactivé | ![](benchmark/optimise-apres-sr.jpg) Après SR |
