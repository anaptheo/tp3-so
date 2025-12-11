<!-- LTeX: language=pt-BR -->

# PAGINADOR DE MEMÓRIA -- RELATÓRIO

1. Termo de compromisso

    Ao entregar este documento preenchiso, os membros do grupo afirmam que todo o código desenvolvido para este trabalho é de autoria própria.  Exceto pelo material listado no item 3 deste relatório, os membros do grupo afirmam não ter copiado material da Internet nem ter obtido código de terceiros.

2. Membros do grupo e alocação de esforço

    Preencha as linhas abaixo com o nome e o email dos integrantes do grupo.  Substitua marcadores `XX` pela contribuição de cada membro do grupo no desenvolvimento do trabalho (os valores devem somar 100%).

    * Ana Paula Pereira Theobald anatheobald@ufmg.br 33%
    * Caio Souza Grossi csgrossi@ufmg.br 33%
    * Pedro Bacelar Rigueira pedrobacelar@ufmg.br 33%

3. Referências bibliográficas

    * Nenhuma além do enunciado e do codigo base fornecido pelo professor.

4. Detalhes de implementação

    1. Estruturas
    
        Vetor global de frames com ponteiro de relogio para a segunda chance, vetor de blocos de disco livres e tabela de processos contendo cada pagina (bloco reservado, frame atual, protecao, flag de sujidade e se ja ha copia valida em disco). Cada processo pode chegar a 256 paginas, limitadas pela faixa UVM.

    2. Controle de acesso
        Páginas novas são mapeadas após zero_fill (ou disk_read, se houver copia em disco) sempre como somente leitura. A primeira escrita em uma pagina gera falta e um mmu_chprot para PROT_READ|PROT_WRITE, marcando a pagina como dirty. O algoritmo de segunda chance remove as permissoes com mmu_chprot(PROT_NONE) e zera o bit referenced; o primeiro acesso depois disso reativa a protecao adequada. A escolha de vitima e deterministica (menor frame livre; caso contrario, varredura circular ate encontrar frame referenced=0), grava a pagina suja em disco com mmu_disk_write e libera o mapeamento com mmu_nonresident antes de reutilizar o frame.

5. Extra: documentação do código

    1. Arquitetura do sistema de memória virtual  

        <img src="diagrama-pto-1.png" style="width: 40%;">  

        O digrama ilustra a interação entre o processo do usuário (azul), a infraestrutura de hardware simulada (amarelo) e o paginador (laranja). Destacam-se o ciclo de tratamento de falhas de página (page faults) e o mecanismo de controle da memória física e armazenamento secundário via MMU.


    2. Fluxograma do funcionamento de um gerenciador de memória virtual  

        <img src="fluxograma.jpeg" style="width: 40%;height:20%">  