// ==========================================
// INICIALIZAÇÃO DE ÍCONES E BUFFERS
// ==========================================
document.addEventListener("DOMContentLoaded", () => {
    if (typeof lucide !== 'undefined') lucide.createIcons();
    adicionarLog("check-circle", "Interface Web iniciada.", "#01b574");
});

const dadosLive = { labels: [], nivel: [], entrada: [], saida: [] };
let historicoReal = []; 
let periodoAtivo = 'live';
let currentNivel = 0, currentEntrada = 0, currentSaida = 0;
let intervalPerformance;

// ==========================================
// CONFIGURAÇÃO DOS GRÁFICOS
// ==========================================
let chartNivel, chartEntrada, chartSaida;

if (typeof Chart !== 'undefined') {
    Chart.defaults.font.family = "'DM Sans', sans-serif";
    Chart.defaults.color = '#a3aed1';

    const baseOptions = {
        responsive: true, maintainAspectRatio: false,
        plugins: { legend: { display: true, position: 'top' } },
        scales: { y: { beginAtZero: true, grid: { borderDash: [5, 5], color: '#eef2f8' } }, x: { grid: { display: false } } },
        animation: { duration: 400 } 
    };

    chartNivel = new Chart(document.getElementById('graficoNivel').getContext('2d'), {
        type: 'line', 
        data: { labels: dadosLive.labels, datasets: [{ label: 'Nível da Água (%)', data: dadosLive.nivel, borderColor: '#4318ff', backgroundColor: 'rgba(67, 24, 255, 0.1)', borderWidth: 3, fill: true, tension: 0.4, pointRadius: 3 }] }, 
        options: { ...baseOptions, scales: { y: { beginAtZero: true, max: 100 } } }
    });
    chartEntrada = new Chart(document.getElementById('graficoEntrada').getContext('2d'), {
        type: 'bar', 
        data: { labels: dadosLive.labels, datasets: [{ label: 'Entrada Acumulada (L)', data: dadosLive.entrada, backgroundColor: '#01b574', borderRadius: 4 }] }, 
        options: baseOptions
    });
    chartSaida = new Chart(document.getElementById('graficoSaida').getContext('2d'), {
        type: 'bar', 
        data: { labels: dadosLive.labels, datasets: [{ label: 'Saída Acumulada (L)', data: dadosLive.saida, backgroundColor: '#ffb547', borderRadius: 4 }] }, 
        options: baseOptions
    });
}

// ==========================================
// ABAS E PERFORMANCE
// ==========================================
window.trocarAba = function(abaDesejada) {
    const abaDash = document.getElementById('aba-dashboard');
    const menuDash = document.getElementById('menu-dash');
    const abaPerf = document.getElementById('aba-performance');
    const menuPerf = document.getElementById('menu-perf');

    // Esconder tudo primeiro
    if(abaDash) abaDash.style.display = 'none';
    if(menuDash) menuDash.classList.remove('ativo');
    if(abaPerf) abaPerf.style.display = 'none';
    if(menuPerf) menuPerf.classList.remove('ativo');

    // Mostrar aba solicitada
    if (abaDesejada === 'dashboard') {
        if(abaDash) abaDash.style.display = 'block';
        if(menuDash) menuDash.classList.add('ativo');
        clearInterval(intervalPerformance);
    } else if (abaDesejada === 'performance') {
        if(abaPerf) abaPerf.style.display = 'block';
        if(menuPerf) menuPerf.classList.add('ativo');
        buscarPerformance(); 
        intervalPerformance = setInterval(buscarPerformance, 3000); 
    }
};

async function buscarPerformance() {
    try {
        const resposta = await fetch('/performance.json?t=' + new Date().getTime());
        if (!resposta.ok) return;
        const diag = await resposta.json();

        const percHeap = ((diag.heap_total - diag.heap_livre) / diag.heap_total) * 100 || 0;
        const percFlash = (diag.flash_uso / diag.flash_total) * 100 || 0;

        if(document.getElementById('mem-heap')) document.getElementById('mem-heap').innerText = (diag.heap_livre / 1024).toFixed(1);
        if(document.getElementById('barra-heap')) document.getElementById('barra-heap').style.width = percHeap + '%';
        if(document.getElementById('mem-flash')) document.getElementById('mem-flash').innerText = diag.flash_uso;
        if(document.getElementById('barra-flash')) document.getElementById('barra-flash').style.width = percFlash + '%';
        if(document.getElementById('cpu-freq')) document.getElementById('cpu-freq').innerText = diag.cpu_freq;
        if(document.getElementById('wifi-rssi')) document.getElementById('wifi-rssi').innerText = diag.wifi_rssi + " dBm";
        if(document.getElementById('task-stack')) document.getElementById('task-stack').innerText = diag.stack_loop;

        if(document.getElementById('t-sensor')) document.getElementById('t-sensor').innerText = diag.t_sensor + " µs";
        if(document.getElementById('t-flash')) document.getElementById('t-flash').innerText = diag.t_flash + " µs";
        if(document.getElementById('t-mqtt')) document.getElementById('t-mqtt').innerText = diag.t_mqtt + " µs";
        if(document.getElementById('t-loop')) document.getElementById('t-loop').innerText = diag.t_loop + " µs";
        if(document.getElementById('t-recon')) document.getElementById('t-recon').innerText = diag.t_recon + " µs";
    } catch (e) { }
}

// ==========================================
// MATEMÁTICA E HISTÓRICO (LITTLEFS)
// ==========================================
async function carregarHistoricoReal() {
    try {
        const resposta = await fetch('/historico.json?t=' + new Date().getTime());
        if (!resposta.ok) throw new Error();
        const json = await resposta.json();
        historicoReal = json.dados || [];
        if (historicoReal.length === 0) {
            adicionarLog("alert-circle", "O ESP32 ainda está a recolher dados.", "#ffb547");
            return;
        }
        processarMatematicaGraficos(periodoAtivo);
    } catch (erro) { 
        adicionarLog("alert-triangle", "Erro ao ler a memória Flash.", "#ee5d50");
    }
}

function processarMatematicaGraficos(periodo) {
    if (historicoReal.length === 0) return;
    let labels = [], nivel = [], entrada = [], saida = [];

    if (periodo === 'dia') {
        historicoReal.forEach(d => { labels.push(d.dataHora.split(' ')[1]); nivel.push(d.nivel); entrada.push(d.entrada); saida.push(d.saida); });
        adicionarLog("bar-chart-2", `Dados do dia carregados com sucesso.`, "#01b574");
    } else {
        const diasAgrupados = {};
        historicoReal.forEach(d => {
            const diaDaData = d.dataHora.split(' ')[0]; 
            if (!diasAgrupados[diaDaData]) diasAgrupados[diaDaData] = { somaEntrada: 0, somaSaida: 0, somaNivel: 0, totalLeituras: 0 };
            diasAgrupados[diaDaData].somaEntrada += d.entrada; diasAgrupados[diaDaData].somaSaida += d.saida; diasAgrupados[diaDaData].somaNivel += d.nivel; diasAgrupados[diaDaData].totalLeituras++;
        });
        for (const dia in diasAgrupados) {
            labels.push(dia); entrada.push(parseFloat(diasAgrupados[dia].somaEntrada.toFixed(2))); saida.push(parseFloat(diasAgrupados[dia].somaSaida.toFixed(2)));
            nivel.push(Math.round(diasAgrupados[dia].somaNivel / diasAgrupados[dia].totalLeituras));
        }
        
        if (periodo === 'semana') adicionarLog("calendar", `Dados da semana consolidados.`, "#4318ff");
        else if (periodo === 'mes') adicionarLog("calendar", `Visão mensal consolidada.`, "#4318ff");
    }

    chartNivel.data.labels = labels; chartNivel.data.datasets[0].data = nivel;
    chartEntrada.data.labels = labels; chartEntrada.data.datasets[0].data = entrada;
    chartSaida.data.labels = labels; chartSaida.data.datasets[0].data = saida;
    chartNivel.update(); chartEntrada.update(); chartSaida.update();
}

window.mudarPeriodo = function(periodo) {
    if (!chartNivel) return;
    periodoAtivo = periodo;
    document.querySelectorAll('.btn-filtro').forEach(btn => btn.classList.remove('ativo'));
    event.target.classList.add('ativo');
    const badge = document.getElementById('badge-live');

    if (periodo === 'live') {
        badge.className = 'badge-live piscar'; badge.innerText = 'Live';
        chartNivel.data.labels = dadosLive.labels; chartNivel.data.datasets[0].data = dadosLive.nivel;
        chartEntrada.data.labels = dadosLive.labels; chartEntrada.data.datasets[0].data = dadosLive.entrada;
        chartSaida.data.labels = dadosLive.labels; chartSaida.data.datasets[0].data = dadosLive.saida;
        chartNivel.update(); chartEntrada.update(); chartSaida.update();
        adicionarLog("activity", "Visualização em Tempo Real ativada.", "#00b5e2");
    } else {
        badge.className = 'badge-live inativo'; badge.innerText = 'Memória Real';
        carregarHistoricoReal();
    }
};

// ==========================================
// LOOP TEMPO REAL E MQTT
// ==========================================
setInterval(() => {
    const agora = new Date().toLocaleTimeString('pt-BR', { hour12: false, hour: '2-digit', minute:'2-digit', second:'2-digit' });
    dadosLive.labels.push(agora); dadosLive.nivel.push(currentNivel); dadosLive.entrada.push(currentEntrada); dadosLive.saida.push(currentSaida);
    
    if (dadosLive.labels.length > 20) { dadosLive.labels.shift(); dadosLive.nivel.shift(); dadosLive.entrada.shift(); dadosLive.saida.shift(); }
    
    if (periodoAtivo === 'live' && chartNivel) { 
        chartNivel.update(); chartEntrada.update(); chartSaida.update(); 
    }
}, 2000);

if (typeof mqtt !== 'undefined') {
    const clientId = 'web_user_' + Math.random().toString(16).substr(2, 8);
    const cliente = mqtt.connect('wss://broker.hivemq.com:8884/mqtt', { keepalive: 60, clientId: clientId });

    const topicoNivel = 'cps/caixa/nivel', topicoFluxoSaida = 'cps/caixa/fluxo', topicoFluxoEntrada = 'cps/caixa/fluxo_entrada', topicoValvula = 'cps/caixa/valvula';

    cliente.on('connect', () => {
        document.getElementById('status-conexao').innerText = 'Online - Recebendo Dados';
        document.getElementById('ponto-mqtt').className = 'ponto ativo';
        cliente.subscribe(topicoNivel); cliente.subscribe(topicoFluxoSaida); cliente.subscribe(topicoFluxoEntrada); cliente.subscribe(topicoValvula);
        adicionarLog("wifi", "Conectado ao Broker MQTT.", "#4318ff");
    });

    cliente.on('message', (topico, mensagem) => {
        const dados = parseFloat(mensagem.toString()); 
        if (topico === topicoNivel) {
            currentNivel = dados; document.getElementById('nivel-agua').innerText = dados; 
            const barra = document.getElementById('barra-nivel');
            if(barra) { barra.style.width = dados + '%'; barra.style.backgroundColor = dados < 20 ? '#ee5d50' : '#00b5e2'; }
        } else if (topico === topicoFluxoSaida) {
            currentSaida = dados; document.getElementById('fluxo-saida').innerText = dados.toFixed(1);
        } else if (topico === topicoFluxoEntrada) {
            currentEntrada = dados; document.getElementById('fluxo-entrada').innerText = dados.toFixed(1);
        } else if (topico === topicoValvula) { 
            atualizarBotaoValvula(mensagem.toString()); 
        }
    });

    window.alternarValvula = function() {
        valvulaAberta = !valvulaAberta;
        cliente.publish(topicoValvula, valvulaAberta ? 'ABRIR' : 'FECHAR');
        adicionarLog("settings-2", `Comando enviado: ${valvulaAberta ? 'ABRIR Válvula' : 'FECHAR Válvula'}`, "#4318ff");
    }
}

let valvulaAberta = true;
function atualizarBotaoValvula(estado) {
    const botao = document.getElementById('botao-valvula');
    if (!botao) return;
    
    if(estado === 'ABERTA' || estado === 'ABRIR') {
        if(!valvulaAberta) adicionarLog("power", "A válvula foi ABERTA.", "#01b574");
        botao.className = 'botao-principal estado-aberto';
        botao.innerHTML = '<i data-lucide="power" class="icone-botao"></i> ESTADO: ABERTA (FECHAR)';
        valvulaAberta = true;
    } else {
        if(valvulaAberta) adicionarLog("power", "A válvula foi FECHADA.", "#ee5d50");
        botao.className = 'botao-principal estado-perigo';
        botao.innerHTML = '<i data-lucide="power" class="icone-botao"></i> ESTADO: FECHADA (ABRIR)';
        valvulaAberta = false;
    }
    if (typeof lucide !== 'undefined') lucide.createIcons();
}

function adicionarLog(iconName, texto, color) {
    const tabela = document.getElementById('lista-logs');
    if(!tabela) return;
    const linha = document.createElement('tr');
    const agora = new Date().toLocaleTimeString('pt-BR', { hour12: false });
    
    // Formatação embutida para evitar problemas se faltar CSS
    linha.innerHTML = `<td style="font-weight:700; color:#2b3674; padding: 10px 15px 10px 0;">${agora}</td><td style="color:#a3aed1; font-weight:500; display:flex; align-items:center;"><i data-lucide="${iconName}" style="color:${color}; width:18px; height:18px; margin-right:8px;"></i> ${texto}</td>`;
    
    tabela.prepend(linha);
    if (typeof lucide !== 'undefined') lucide.createIcons();
    if(tabela.children.length > 5) tabela.removeChild(tabela.lastChild); // Mantém os 5 mais recentes
}
// ==========================================
// CONFIGURAÇÃO DO SISTEMA (RESET WI-FI)
// ==========================================
window.resetarWiFi = function() {
    const confirmacao = confirm("ATENÇÃO: Isto irá apagar a rede Wi-Fi salva e o sistema será reiniciado. Terás de aceder pelo telemóvel à rede 'WaterInBox_Config' para configurar a nova rede. Desejas prosseguir?");
    
    if (confirmacao) {
        adicionarLog("wifi-off", "Pedido de exclusão de Wi-Fi enviado.", "#ee5d50");
        fetch('/reset_wifi', { method: 'POST' })
            .then(response => {
                if (response.ok) {
                    alert("Credenciais apagadas! O dispositivo vai reiniciar. Conecta-te à rede 'WaterInBox_Config'.");
                    // Desabilita a interface pois o ESP32 vai desligar da rede
                    document.body.style.opacity = "0.5";
                }
            })
            .catch(err => console.error("Erro ao resetar Wi-Fi:", err));
    }
}