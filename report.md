# PAGINADOR DE MEMORIA -- RELATORIO

1. Termo de compromisso

    Ao entregar este documento, os integrantes do grupo declaram que o codigo desenvolvido para este trabalho e de autoria propria. Exceto pelo material listado em Referencias, nao houve copia de terceiros.

2. Membros do grupo e alocacao de esforco

    * Preencha nome <email> 100%

3. Referencias bibliograficas

    * Nenhuma alem do enunciado e do codigo base fornecido pelo professor.

4. Detalhes de implementacao

    1. Estruturas: vetor global de frames com ponteiro de relogio para a segunda chance, vetor de blocos de disco livres e tabela de processos contendo cada pagina (bloco reservado, frame atual, protecao, flag de sujidade e se ja ha copia valida em disco). Cada processo pode chegar a 256 paginas, limitadas pela faixa UVM.
    2. Controle de acesso: paginas novas sao mapeadas apos zero_fill (ou disk_read, se houver copia em disco) sempre como somente leitura. A primeira escrita em uma pagina gera falta e um mmu_chprot para PROT_READ|PROT_WRITE, marcando a pagina como dirty. O algoritmo de segunda chance remove as permissoes com mmu_chprot(PROT_NONE) e zera o bit referenced; o primeiro acesso depois disso reativa a protecao adequada. A escolha de vitima e deterministica (menor frame livre; caso contrario, varredura circular ate encontrar frame referenced=0), grava a pagina suja em disco com mmu_disk_write e libera o mapeamento com mmu_nonresident antes de reutilizar o frame.
