#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <chrono>

using namespace std;
using namespace this_thread;
using namespace chrono;


// Aqui tinha uma classe barreira pra sincronização, mas temos contadores de tempo que cada thread herda.

enum Estado {
     PRONTO, 
     EXECUTANDO, 
     BLOQUEADO, 
     FINALIZADO 
    };

struct Processo {
    string id = " ";
    int tempoChegada, exec1, tempoEspera, exec2;
    bool temBloqueio, bloqueioExecutado, emFila;
    Estado estado;
    int tInicioExec, tFim, tEsperaTotal, trocas, tempoBloqRestante, tExecTotal;
    
    // Construtor da struct, inicialização;
    Processo() : tempoChegada(0), exec1(0), tempoEspera(0), exec2(0),
                 temBloqueio(false), bloqueioExecutado(false), emFila(false),
                 estado(PRONTO), tInicioExec(-1), tFim(0), tEsperaTotal(0), trocas(0),
                 tempoBloqRestante(0), tExecTotal(0) {}
};

class Simulador { 
private:
    int quantumFixo; // Tamanho do quantum fixo em ticks, para o caso de usarmos quantos fixo e nao dinamico
    
    int numNucleos; // Numeros de threads a serem simuladas pelo vetor de nucleos

    bool usarQuantumDin; // Pra função `calcQuantumDin` nao for dinamico, alteramos quanto de quantum usar de acordo com N processos

    /*
    Usamos os tipos atomic generico pras threads conseguirem ler e modificar as variaveis sem mutex 
    (Pois se condição de corrida ocorrer, cada operação ocorre como se fosse uma,
    nao consegue se dividir em duas operações diferentes)
    */
    atomic<int> tGlobal; // Tempo global em ticks (Inteiro) pra todas as threads

    deque<int> filaProntos; // Fila de prontos pra serem executados (Usamos deque ao inves de queue pra maior funcionalidade)

    vector<int> filaBloq, nucleos; // Lista de processos bloqueados e vetor de nucleo seria o indice do processo em execução (filaExec)

    map<int, vector<int>> gantt; 

    mutex mtx, mtxGantt; 

    atomic<bool> simAtiva; // Pra thread encerrar (true se estiver ativa)

    atomic<int> tickCnt, numFinalizados; // Contador de tick pra acordar os trabalhos uma vez por tick (Tem 10 000 de ticks pra cada segundo)


   // Melhor pro quantum dinamico (Quando alocamos processo)
   // A cada tick decrementamos o quantum restante (começa no fixo e decrementa ate 0 pro processo sair pra um outro entrar)
    vector<int> quantumRestante; // Quantum por processo.

    vector<Processo> procs; // Nosso vetor de processos com estado e tempo e id pra cada
    
    // Usado para alocar processos -> executar
    int calcQuantumDin() { 
        const size_t sz = filaProntos.size(); // deque.size retorna size_t

        if (sz <= 2) return quantumFixo; // Nao precisamos de quantum dinamico se tem poucos prontos pra serem executados

        if (sz <= 5) return max(1, quantumFixo - 1); // Cada processo tem que pelo menos ter 1 de quantum, se quantum for 0 o processo nao avança;

        return max(1, quantumFixo - 2); // Reduzimos em 2 no quantum se a fila for maior que 6, para o quantum ser menor e ter maior rotatividade
    }
    
    // Usado para execução 
    void verChegadas() {
        lock_guard<mutex> lk(mtx); // Lock pra garantir que so uma thread por vez chegada nessa func (pelo loop) modifique a fila de prontos
     
        for (size_t i = 0; i < procs.size(); i++) { // 
            // Se o tempo que o processo chegou for igual ao tick global, e estiver pronto pra executar, e nao estiver em fila (Já executando)
            // Entao podemos adicionar a fila de prontos, colocar o processo em fila, e o tempo de exec total pra controle de espera (Printar)
            if (procs[i].tempoChegada == tGlobal && procs[i].estado == PRONTO && !procs[i].emFila) { 
                filaProntos.push_back(i);
                procs[i].emFila = true;
                procs[i].tExecTotal = procs[i].exec1 + procs[i].exec2; 
                // Nao precisamos marcar como pronto estado pois ele ja esta pronto
            }
        }
    } // Note que, nao precisamos dar unlock(), ja que lock_guard() faz isso automaticamente quando saimos do escopo dessa funçao e lock guard é destruido, 
     // E em destruição, lock guard faz mutex.unlock
    
    // Usado para funçao executar() para desbloquear processos pendentes em um unico contador de tempo tick.
    void verDesbloq() {
        lock_guard<mutex> lk(mtx);
        // Tiramos da fila de bloqueado se ele tiver na fila (entre o começo e o fim), e se o tempo de bloqueado for = 0 ou menor que 0 pra alguns casos que nao consiguimos tirar.
        filaBloq.erase(remove_if(filaBloq.begin(), filaBloq.end(), [this](int idx) { // func lambda anonima pra cada indice da fila de bloqueadas, serve como ponteiro da classe [this] pra acessar processos e fila de prontos
            procs[idx].tempoBloqRestante--;
            if (procs[idx].tempoBloqRestante <= 0) {
                procs[idx].estado = PRONTO; // O processo esta pronto denovo e nao bloqueado.
                filaProntos.push_back(idx);
                procs[idx].emFila = true;
                return true; // Estamos dentro de uma função anonima nesse escopo, que e o remove_if()
            }
            return false;
        }), filaBloq.end()); 
    }
    
    // Função usada em executar() depois de ver as chegadas e ver os desbloqueios
    void alocarProcs() {
        lock_guard<mutex> lk(mtx); // Sempre usando o mutex em toda função pois no final ele da unlock() do destruidor da função 
        for (int n = 0; n < numNucleos; n++) { // pra cada thread

            // Apenas alocamos caso a thread estiver vazia/Nao em execução (veja threadNucleo()) e tem processos prontos na fila (Nao esta vazia)
            if (nucleos[n] == -1 && !filaProntos.empty()) { 
                int idx = filaProntos.front(); // Na frente da fila de prontos (Estamos usando um fifo basicamente)

                filaProntos.pop_front(); // Removemos da fila de prontos pra execução

                procs[idx].emFila = false; // Nao esta mais em fila

                nucleos[n] = idx; // nucleos é um vetor de threads em execução.

                procs[idx].estado = EXECUTANDO;

                quantumRestante[n] = usarQuantumDin ? calcQuantumDin() : quantumFixo; // Quantum restante pra essa thread

                if (procs[idx].tInicioExec == -1) procs[idx].tInicioExec = tGlobal; // Na primeira vez que o processo roda tempos que o tempo de execuçaõ dele é igual ao tempo global (chegou agora)
            }
        }
    }
    
    // Usado na função executar()
    void threadNucleo(int nId) { // Id é o iterador (Chamamos em um loop de 0 a N nucleos)
        int lastTick = -1;
        while (simAtiva) { // Todos os processos estao ativos default, condição de parada é a simulação acabar, todos os processos estarem finalizados.
            int currentTick = tickCnt.load();
            if (currentTick == lastTick) { // Contador nao avançou entao dormimos por 1 microsegundo e quebramos o loop ()
                sleep_for(microseconds(1));
                continue;
            }
            lastTick = currentTick; // Teve um avanço de tick logo ultimoTick igual tick atual
            
            int idx = nucleos[nId]; // nucleo é um vetor de threads em exec, id é o indice dele.
            
            // Pro gant atualizar (Registrar que nucleo esta rodando nesse tick)
            {
                lock_guard<mutex> lg(mtxGantt);
                if (idx == -1) {  // Caso o nucleo esteja vazio e nao esta executando
                    gantt[nId].push_back(-1);
                } else {
                    gantt[nId].push_back(idx);
                }
            }
            // Caso tenha um processo alocado e executando nesse nucleo
            if (idx != -1) {
                {
                    lock_guard<mutex> lk(mtx);
                    procs[idx].tExecTotal--; // Decrementamos 1 no tempo de execução total do processo (Tempo restante)

                    quantumRestante[nId]--; // Decrementamos 1 no quantum restante do processo por vez 

                    // Só decrementamos 1 por vez para ter sincronismo com o tick global, e cada nucleo executa um processo por vez nessa simulação
                    // Logo, cada thread controla o quantum apenas do processo executado no momento T.
                    
                    bool precBloq = false;
                    // Checando se o processo tem bloqueio mas nao executou ainda

                    if (procs[idx].temBloqueio && !procs[idx].bloqueioExecutado) {

                        int tExec = procs[idx].exec1 + procs[idx].exec2 - procs[idx].tExecTotal;

                      // Mecanismo de proteção, caso o tempo total de execução (tempo1 + tempo2 + tempo total) for maior que o tempo de exec 1,
                      //  tem que ser bloqueado pra evitar deadlock.
                        if (tExec >= procs[idx].exec1) precBloq = true;
                    }
                    
                    if (procs[idx].tExecTotal <= 0) { // Acabou
                        procs[idx].estado = FINALIZADO;
                        procs[idx].tFim = tGlobal + 1;
                        nucleos[nId] = -1; // Esta vazio, pode ser alocado denovo, veja alocarProcs()
                        numFinalizados++;
                    } else if (precBloq) { // Precisa ser bloqueado
                        procs[idx].estado = BLOQUEADO; 

                        procs[idx].bloqueioExecutado = true;

                        procs[idx].tempoBloqRestante = procs[idx].tempoEspera; // Veja carregarProcs()

                        filaBloq.push_back(idx); // Entra na fila de bloqueados 

                        nucleos[nId] = -1; // Esta livre pra ser usado, veja alocarProcs()

                        procs[idx].trocas++; // Mais uma troca de contexto pra ele

                    } else if (quantumRestante[nId] <= 0) { // Quantum do processo acabou, troca de contexto, esta pronto pra ser executado de novo assumindo que nao foi bloqueado.
                        procs[idx].estado = PRONTO;
                        filaProntos.push_back(idx);
                        procs[idx].emFila = true; // Volta pra fila de prontos

                        nucleos[nId] = -1; 

                        procs[idx].trocas++;
                    }
                }
            }
        }
    }
    
    bool todosFinalizados() {
        return numFinalizados.load() >= procs.size(); // Quando todos os processos entraram no contador de finalizados.
    }
    
    void calcEsperaTotal() {
        for (auto& p : procs) {
            if (p.estado == FINALIZADO) {
                int ta = p.tFim - p.tempoChegada;
                p.tEsperaTotal = ta - p.tExecTotal - (p.temBloqueio ? p.tempoEspera : 0);
            }
        }
    }
    
public:
    Simulador(int q, bool din, int nuc) : quantumFixo(q), numNucleos(nuc), usarQuantumDin(din),
        tGlobal(0), simAtiva(true), tickCnt(0), numFinalizados(0) {
        nucleos.resize(nuc, -1);
        quantumRestante.resize(nuc, 0);
    }
    
    void carregarProcs(const string& arq) {
        ifstream f(arq);
        if (!f.is_open()) { cout << "Erro ao abrir: " << arq << endl; return; }
        string ln;
        while (getline(f, ln)) {
            if (ln.empty() || ln[0] == '#') continue;
            Processo p;
            stringstream ss(ln);
            string tk;
            getline(ss, tk, '|'); p.id = tk; p.id.erase(0, p.id.find_first_not_of(" \t")); p.id.erase(p.id.find_last_not_of(" \t") + 1);
            getline(ss, tk, '|'); p.tempoChegada = stoi(tk);
            getline(ss, tk, '|'); p.exec1 = stoi(tk);
            getline(ss, tk, '|'); tk.erase(0, tk.find_first_not_of(" \t")); tk.erase(tk.find_last_not_of(" \t") + 1); p.temBloqueio = (tk == "S" || tk == "s");
            getline(ss, tk, '|'); p.tempoEspera = stoi(tk);
            getline(ss, tk, '|'); p.exec2 = stoi(tk);
            p.estado = PRONTO;
            procs.push_back(p);
        }
        f.close();
        cout << "Carregados " << procs.size() << " processos.\n";
    }
    
    void executar() {
        vector<thread> ths; // Vetor de threads pra cada nucleo apenas nesse escopo
        for (int i = 0; i < numNucleos; i++) ths.emplace_back(&Simulador::threadNucleo, this, i);
        
        while (!todosFinalizados()) {
            verChegadas();
            verDesbloq();
            alocarProcs();
            
            tickCnt++;
            
           sleep_for(milliseconds(1));
            
            tGlobal++;
            if (tGlobal > 10000) { 
                cout << "Limite excedido!\n"; 
                break; 
            } 
        }
        
        simAtiva = false;
        for (auto& t : ths) 
          if (t.joinable()) t.join();
        calcEsperaTotal();
    }
    
    void exibirResultados() {
        cout << "\n========== RESULTADOS ==========\n\nMETRICAS:\n";
        cout << left << setw(8) << "ID" << setw(10) << "Chegada" << setw(10) << "Fim" << setw(10) << "Espera" << setw(12) << "Turnaround" << setw(8) << "Trocas\n";
        cout << string(58, '-') << "\n";
        
        double somaEsp = 0, somaTA = 0;
        int somaTroc = 0;
        for (auto& p : procs) {
            int ta = p.tFim - p.tempoChegada;
            cout << left << setw(8) << p.id << setw(10) << p.tempoChegada << setw(10) << p.tFim << setw(10) << p.tEsperaTotal << setw(12) << ta << setw(8) << p.trocas << "\n";
            somaEsp += p.tEsperaTotal; somaTA += ta; somaTroc += p.trocas;
        }
        
        cout << string(58, '-') << "\n";
        cout << "Espera media: " << fixed << setprecision(2) << somaEsp / procs.size() << "\n";
        cout << "Turnaround medio: " << somaTA / procs.size() << "\n";
        cout << "Trocas contexto: " << somaTroc << "\n";
        
        cout << "\n========== GANTT ==========\n";
        int tMax = 0;
        for (int n = 0; n < numNucleos; n++) tMax = max(tMax, (int)gantt[n].size());
        
        for (int n = 0; n < numNucleos; n++) {
            bool temAtiv = false;
            for (auto& idx : gantt[n]) if (idx != -1) { temAtiv = true; break; }
            if (temAtiv) {
                cout << "N" << (n + 1) << ": | ";
                for (auto& idx : gantt[n]) {
                    if (idx == -1) cout << "-";
                    else cout << procs[idx].id;
                    cout << " | ";
                }
                cout << "\n";
            }
        }
        
        cout << "T:  ";
        for (int t = 0; t < tMax; t++) cout << setw(3) << t << " ";
        cout << "\n\n========== UTILIZACAO ==========\n";
        
        int nucAtiv = 0;
        for (int n = 0; n < numNucleos; n++) {
            int tOcup = 0;
            for (auto& idx : gantt[n]) if (idx != -1) tOcup++;
            if (tOcup > 0) {
                nucAtiv++;
                double util = (double)tOcup / gantt[n].size() * 100.0;
                cout << "N" << (n + 1) << ": " << fixed << setprecision(2) << util << "% (" << tOcup << "/" << gantt[n].size() << ")\n";
            }
        }
        if (nucAtiv < numNucleos) cout << "N" << (nucAtiv + 1) << "-N" << numNucleos << ": 0.00%\n";
        cout << "\n";
    }
};

int main(int argc, char* argv[]) { // Numero de argumentos passados na linha de comando
    string arq = "processos.txt";
    int quantum = 2, nucleos = 2;
    bool quantumDin = false;
    
    cout << "========== SIMULADOR ROUND ROBIN ==========\n\n";
    cout << "Trabalho de M2: Sistemas Opeacionais " << endl;
    cout << "Alunos: Mauro, Celso, Luan " << endl;
    
    if (argc > 1) arq = argv[1];
    else {
        cout << "Arquivo (padrao: processos.txt): ";
        string e; getline(cin, e);
        if (!e.empty()) arq = e;
    }
    
    cout << "Nucleos (padrao: 2): "; string e; getline(cin, e);
    if (!e.empty()) nucleos = stoi(e);
    
    cout << "Quantum (padrao: 2): "; getline(cin, e);
    if (!e.empty()) quantum = stoi(e);
    
    cout << "Quantum dinamico? (S/N): "; getline(cin, e);
    if (!e.empty() && (e[0] == 'S' || e[0] == 's')) quantumDin = true;
    
    cout << "\nConfig: Arquivo=" << arq << ", Nucleos=" << nucleos << ", Quantum=" << quantum << (quantumDin ? " (din)" : " (fixo)") << "\n\nIniciando...\n";
    
    Simulador sim(quantum, quantumDin, nucleos);
    sim.carregarProcs(arq);
    sim.executar();
    sim.exibirResultados();
    
    return 0;
}
