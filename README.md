Estarei colocando aqui o código para o esp32 normal, não o mini, posteriormente vou explicar como modificar o código para o mini
É trivial, contudo a arquitetura arm do meu notebook dificulta tudo.


Para rodar o código será necessário utilizar algum editor de código que rode a ide do arduino. Recomendo usar o PlatformIO do vscode (é o que eu estou usando).
Apenas abra a pasta onde está o código no vscode, espere o PlatformIO reconhecer e baixar o que for necessário. Demorará bastante mas vai funcionar. Contudo abra apenas um arquivo por aba do vscode pois se tiver mais de um arquivo platformio.ini na aba o PlatformIO dirá que está com erro.


O código do c3 mini está disponível, vocês terão que fazer upload dele na placa ainda, vou deixar um arquivo de imagem mostrando em quais pinos encaixar os sensores, tanto na placa do esp normal quanto no mini. Após vocês confirmarem que o c3 mini está funcionando irei atualizar somente o código dele, caso eu pense numa atualização pertinente, mas por enquanto testem os códigos atuais.

Após abrir o arquivo no vscode e o PlatformIO reconhecer, dê um build (símbolo de V no canto inferior direito da tela, na barra azul) e quando der a mensagem de sucesso clique na seta ao lado do build e o código será enviado para a placa, não esqueça de segurar o botão de boot enquanto o upload acontencer e o botão reset após o upload estar completo.

Todo código que estiver no branch master será para a placa normal do esp32, caso tenha tempo criarei um branch com o código adaptado para o esp32 c3 mini, o teste e compilação para o c3 ficará por conta de vocês pelos motivos que falei no inicio do documento.

Continuem checando o README a cada commit, vou atualizar se for necessário. Quaisquer dúvidas e possíveis erros, perguntem a mim ou ao Gemini (na versão pro se possível).
