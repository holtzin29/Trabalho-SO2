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

class Barrier {
private:
    mutex mtx;
    condition_variable cv;
    int count;
    int generation;
    int numThreads;

public:
    explicit Barrier(int numThreads) : count(numThreads), generation(0), numThreads(numThreads) {}
    
    void arrive_and_wait() {
        unique_lock<mutex> lock(mtx);
        int gen = generation;
        
        if (--count == 0) {
            generation++;
            count = numThreads;
            cv.notify_all();
        } else {
            cv.wait(lock, [this, gen] { return gen != generation; });
        }
    }
};

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
    
    Processo() : tempoChegada(0), exec1(0), tempoEspera(0), exec2(0),
                 temBloqueio(false), bloqueioExecutado(false), emFila(false),
                 estado(PRONTO), tInicioExec(-1), tFim(0), tEsperaTotal(0), trocas(0),
                 tempoBloqRestante(0), tExecTotal(0) {}
};

class Simulador {
private:
    int quantumFixo, numNucleos;
    bool usarQuantumDin;
    atomic<int> tGlobal;
    vector<Processo> procs;
    deque<int> filaProntos;
    vector<int> filaBloq, nucleos;
    map<int, vector<int>> gantt;  
    mutex mtx, mtxGantt;
    atomic<bool> simAtiva;
    atomic<int> tickCnt, numFinalizados;
    vector<int> quantumRestante; 
    
    int calcQuantumDin() {
        const size_t sz = filaProntos.size();
        if (sz <= 2) return quantumFixo;
        if (sz <= 5) return max(1, quantumFixo - 1);
        return max(1, quantumFixo - 2);
    }
    
    void verChegadas() {
        lock_guard<mutex> lk(mtx);
        for (size_t i = 0; i < procs.size(); i++) {
            if (procs[i].tempoChegada == tGlobal && procs[i].estado == PRONTO && !procs[i].emFila) {
                filaProntos.push_back(i);
                procs[i].emFila = true;
                procs[i].tExecTotal = procs[i].exec1 + procs[i].exec2;
            }
        }
    }
    
    void verDesbloq() {
        lock_guard<mutex> lk(mtx);
        filaBloq.erase(remove_if(filaBloq.begin(), filaBloq.end(), [this](int idx) {
            procs[idx].tempoBloqRestante--;
            if (procs[idx].tempoBloqRestante <= 0) {
                procs[idx].estado = PRONTO;
                filaProntos.push_back(idx);
                procs[idx].emFila = true;
                return true;
            }
            return false;
        }), filaBloq.end());
    }
    
    void alocarProcs() {
        lock_guard<mutex> lk(mtx);
        for (int n = 0; n < numNucleos; n++) {
            if (nucleos[n] == -1 && !filaProntos.empty()) {
                int idx = filaProntos.front();
                filaProntos.pop_front();
                procs[idx].emFila = false;
                nucleos[n] = idx;
                procs[idx].estado = EXECUTANDO;
                quantumRestante[n] = usarQuantumDin ? calcQuantumDin() : quantumFixo;
                if (procs[idx].tInicioExec == -1) procs[idx].tInicioExec = tGlobal;
            }
        }
    }
    
    void threadNucleo(int nId) {
        int lastTick = -1;
        while (simAtiva) {
            int currentTick = tickCnt.load();
            if (currentTick == lastTick) {
                sleep_for(microseconds(1));
                continue;
            }
            lastTick = currentTick;
            
            int idx = nucleos[nId];
            
            {
                lock_guard<mutex> lg(mtxGantt);
                if (idx == -1) {
                    gantt[nId].push_back(-1);
                } else {
                    gantt[nId].push_back(idx);
                }
            }
            
            if (idx != -1) {
                {
                    lock_guard<mutex> lk(mtx);
                    procs[idx].tExecTotal--;
                    quantumRestante[nId]--;
                    
                    bool precBloq = false;
                    if (procs[idx].temBloqueio && !procs[idx].bloqueioExecutado) {
                        int tExec = procs[idx].exec1 + procs[idx].exec2 - procs[idx].tExecTotal;
                        if (tExec >= procs[idx].exec1) precBloq = true;
                    }
                    
                    if (procs[idx].tExecTotal <= 0) {
                        procs[idx].estado = FINALIZADO;
                        procs[idx].tFim = tGlobal + 1;
                        nucleos[nId] = -1;
                        numFinalizados++;
                    } else if (precBloq) {
                        procs[idx].estado = BLOQUEADO;
                        procs[idx].bloqueioExecutado = true;
                        procs[idx].tempoBloqRestante = procs[idx].tempoEspera;
                        filaBloq.push_back(idx);
                        nucleos[nId] = -1;
                        procs[idx].trocas++;
                    } else if (quantumRestante[nId] <= 0) {
                        procs[idx].estado = PRONTO;
                        filaProntos.push_back(idx);
                        procs[idx].emFila = true;
                        nucleos[nId] = -1;
                        procs[idx].trocas++;
                    }
                }
            }
        }
    }
    
    bool todosFinalizados() {
        return numFinalizados.load() >= procs.size();
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
        vector<thread> ths;
        for (int i = 0; i < numNucleos; i++) ths.emplace_back(&Simulador::threadNucleo, this, i);
        
        while (!todosFinalizados()) {
            verChegadas();
            verDesbloq();
            alocarProcs();
            
            tickCnt++;
            
           sleep_for(milliseconds(1));
            
            tGlobal++;
            if (tGlobal > 10000) { cout << "Limite excedido!\n"; break; }
        }
        
        simAtiva = false;
        for (auto& t : ths) if (t.joinable()) t.join();
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

int main(int argc, char* argv[]) {
    string arq = "processos.txt";
    int quantum = 2, nucleos = 2;
    bool quantumDin = false;
    
    cout << "========== SIMULADOR ROUND ROBIN ==========\n\n";
    cout << "Trabalho de M2: Sistemas Opeacionais " << endl;
    cout << "Alunos: Mauro, Celso, " << endl;
    
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
