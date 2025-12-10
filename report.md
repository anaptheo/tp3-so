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

    *   Arpaci-Dusseau, R. H., & Arpaci-Dusseau, A. C. (2018). *Operating Systems: Three Easy Pieces*. Arpaci-Dusseau Books. Disponível em: <http://pages.cs.wisc.edu/~remzi/OSTEP/>.
    *   Silberschatz, A., Galvin, P. B., & Gagne, G. (2018). *Operating System Concepts*. 10th ed. Wiley.

4. Detalhes de implementação

    1. Descreva e justifique as estruturas de dados utilizadas em sua solução.

       Utilizamos três estruturas principais para gerenciar a memória:
       
       *   `struct Page`: Representa uma página virtual de um processo. Armazena o endereço virtual (`vaddr`), o quadro físico (`frame`), o bloco em disco (`block`), e flags de estado essenciais: `resident` (se está na memória), `referenced` (bit de referência para o algoritmo de segunda chance), `dirty` (bit de modificação), `initialized` (se o conteúdo é válido) e `disk_valid` (otimização para indicar se o bloco em disco contém dados válidos). A escolha de uma lista encadeada para as páginas permite alocação dinâmica e esparsa, economizando memória para o paginador.
       *   `struct Process`: Mantém o identificador do processo (`pid`) e a lista de suas páginas. Os processos são organizados em uma lista encadeada global, permitindo o gerenciamento de múltiplos processos conectados à infraestrutura.
       *   `struct Frame`: Representa um quadro de memória física. Armazena se está em uso, e qual processo (`pid`) e endereço virtual (`vaddr`) o ocupam. Essa estrutura facilita a identificação reversa (quadro -> página) necessária durante o processo de substituição de páginas (eviction).

       Além destas, utilizamos arrays globais para o mapa de quadros (`g_frames`) e blocos de disco (`g_blocks`), e um `pthread_mutex_t` global para garantir a exclusão mútua e thread-safety de todas as operações do paginador.

    2. Descreva o mecanismo utilizado para controle de acesso e modificação às páginas.

       O controle de acesso e a política de substituição (Segunda Chance) são implementados manipulando as permissões de proteção da memória (`prot`) via `mmu_chprot`:

       *   **Detecção de Acesso (Leitura/Escrita):** Quando uma página é carregada na memória, ela recebe inicialmente permissão `PROT_READ`. Se o processo tentar escrever nela, ocorrerá uma falha de proteção. O paginador intercepta essa falha, marca a página como `dirty` e `referenced`, e atualiza a permissão para `PROT_READ | PROT_WRITE`.
       *   **Algoritmo de Segunda Chance:** Quando é necessário liberar um quadro e não há livres, o paginador percorre os quadros circularmente (Clock). Se a página no quadro atual tiver o bit `referenced` ligado, o paginador lhe dá uma "segunda chance": zera o bit `referenced` e remove todas as permissões de acesso (`PROT_NONE`). Isso garante que o próximo acesso à página gere uma falha, permitindo ao paginador saber que ela foi referenciada novamente (restaurando as permissões e o bit `referenced`). Se o bit `referenced` já for zero, a página é eleita para substituição (eviction).
       *   **Otimização de Disco:** Adicionamos um flag `disk_valid`. Páginas que foram apenas alocadas e zeradas (`mmu_zero_fill`), mas nunca escritas no disco (evictadas limpas), não precisam ser lidas do disco quando trazidas de volta; basta zerá-las novamente. Isso economiza operações de I/O.
