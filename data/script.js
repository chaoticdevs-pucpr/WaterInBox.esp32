// Configuração do Gráfico WaterInBox
const ctx = document.getElementById('graficoConsumo').getContext('2d');
const graficoConsumo = new Chart(ctx, {
    type: 'line',
    data: {
        labels: ['00:00', '04:00', '08:00', '12:00', '16:00', '20:00', '23:59'],
        datasets: [{
            label: 'Consumo (L)',
            data: [10, 5, 80, 45, 60, 110, 30],
            borderColor: '#223bc2',
            backgroundColor: 'rgba(143, 165, 241, 0.15)',
            borderWidth: 3,
            fill: true,
            tension: 0.4,
            pointRadius: 0
        }]
    },
    options: {
        responsive: true,
        plugins: { legend: { display: false } },
        scales: {
            y: { beginAtZero: true, grid: { color: '#e2e8f0' } },
            x: { grid: { display: false } }
        }
    }
});

const cliente = mqtt.connect('wss://broker.hivemq.com:8884/mqtt');

const topicoNivel = 'cps/caixa/nivel';
const topicoFluxo = 'cps/caixa/fluxo';
const topicoFluxoEntrada = 'cps/caixa/fluxo_entrada'; 
const topicoValvula = 'cps/caixa/valvula';

cliente.on('connect', () => {
    document.getElementById('status-conexao').innerText = 'Telemetria Ativa';
    document.getElementById('ponto-mqtt').className = 'ponto ativo';
    
    cliente.subscribe(topicoNivel);
    cliente.subscribe(topicoFluxo);
    cliente.subscribe(topicoFluxoEntrada);
    cliente.subscribe(topicoValvula);
    
    limparLogs();
    adicionarLog("WaterInBox online. Monitorando fluxo e nível.");
});

cliente.on('message', (topico, mensagem) => {
    const dados = mensagem.toString();
    
    if (topico === topicoNivel) {
        document.getElementById('nivel-agua').innerText = dados;
        document.getElementById('barra-nivel').style.width = dados + '%';
        
        if(dados < 20) {
            document.getElementById('barra-nivel').style.backgroundColor = '#ef4444';
        } else {
            document.getElementById('barra-nivel').style.backgroundColor = '#223bc2';
        }
    } 
    else if (topico === topicoFluxo) {
        document.getElementById('fluxo-saida').innerText = dados;
    }
    else if (topico === topicoFluxoEntrada) {
        document.getElementById('fluxo-entrada').innerText = dados;
    }
    else if (topico === topicoValvula) {
        atualizarBotaoValvula(dados);
    }
});

let valvulaAberta = true;
function alternarValvula() {
    valvulaAberta = !valvulaAberta;
    const comando = valvulaAberta ? 'ABRIR' : 'FECHAR';
    cliente.publish(topicoValvula, comando);
    
    const acao = valvulaAberta ? 'Abertura' : 'Bloqueio';
    adicionarLog(`Comando enviado: ${acao} da entrada de água.`);
}

function atualizarBotaoValvula(estado) {
    const botao = document.getElementById('botao-valvula');
    if(estado === 'ABERTA' || estado === 'ABRIR') {
        botao.className = 'botao-principal estado-aberto';
        botao.innerHTML = 'ESTADO: ABERTA (CLIQUE PARA FECHAR)';
        valvulaAberta = true;
    } else {
        botao.className = 'botao-principal estado-perigo';
        botao.innerHTML = 'ESTADO: FECHADA (CLIQUE PARA ABRIR)';
        valvulaAberta = false;
    }
}

function limparLogs() { document.getElementById('lista-logs').innerHTML = ''; }

function adicionarLog(texto) {
    const tabela = document.getElementById('lista-logs');
    const linha = document.createElement('tr');
    const agora = new Date().toLocaleTimeString('pt-BR', { hour12: false });
    
    linha.innerHTML = `<td class="celula-tempo">${agora}</td><td>${texto}</td>`;
    tabela.prepend(linha);
}