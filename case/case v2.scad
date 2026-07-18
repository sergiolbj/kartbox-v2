/* ====================================================================
   CASE PARAMÉTRICO COMPACTO KARTBOX (V29 - COLUNAS CORRIGIDAS - 4 BOTÕES)
   ==================================================================== */

$fn = 60; 

// ====================================================================
// 1. CONTROLES DE CRESCIMENTO E TAMANHO GERAL
// ====================================================================
extensao_horizontal_x = 4.00;  
extensao_vertical_y   = 0.00;  
caixa_profundidade_interna = 12.50; 
parede_espessura      = 2.00; 
raio_canto_externo    = 4.50; 

// ====================================================================
// 2. CONFIGURAÇÕES FIXAS DOS COMPONENTES (VISOR, BOTÕES, GPS, ÍMÃS)
// ====================================================================
visor_vidro_x     = 114.40; 
visor_vidro_y     = 66.80;  
visor_espessura   = 2.00; 
visor_ativa_x     = 93.60;  
visor_ativa_y     = 56.16;  
folga_visor       = 0.30; 

botao_pino_diametro = 4.20; 
botao_corpo_medida  = 6.40; 
botao_profundidade  = 1.50; 
qtd_botoes          = 4;     // 4 Botões com espaçamento automático
margin_botoes       = 6.50; 

gps_x                = 16.00; 
gps_y                = 12.00; 
folga_gps            = 0.40;  
gps_shift_x          = 0.00; 

ima_x                = 30.00; 
ima_y                = 10.00; 
ima_z                = 3.00;  
folga_ima            = 0.35;  
dist_lateral_ima     = 12.00; 

// CONFIGURAÇÃO DO INSERT DE LATÃO M3
diametro_furo_insert = 4.20; // Diâmetro ideal para derreter o insert M3 no plástico

// ====================================================================
// 3. MATEMÁTICA AUTOMÁTICA
// ====================================================================
janela_x = visor_ativa_x + 0.4; 
janela_y = visor_ativa_y + 0.4;
berco_x  = visor_vidro_x + folga_visor;
berco_y  = visor_vidro_y + folga_visor;

borda_lateral_min = 1.50;

caixa_externa_x = berco_x + (borda_lateral_min * 2) + (extensao_horizontal_x * 2);
caixa_externa_y = berco_y + (parede_espessura * 2) + margin_botoes + (extensao_vertical_y * 2); 

espessura_base_traseira = ima_z + 1.20; 
caixa_profundidade      = caixa_profundidade_interna + espessura_base_traseira;

dist_furo   = 4.2;
pos_furos_x = [dist_furo, caixa_externa_x - dist_furo];
pos_furos_y = [dist_furo, caixa_externa_y - dist_furo];

pos_x_visor = (caixa_externa_x - berco_x) / 2;
pos_y_visor = margin_botoes + parede_espessura + extensao_vertical_y;

dim_gps_x      = gps_x + folga_gps;
dim_gps_y      = gps_y + folga_gps;
pos_x_gps_base = (caixa_externa_x - dim_gps_x) / 2 + gps_shift_x;
pos_y_gps_base = caixa_externa_y - parede_espessura - dim_gps_y;

dim_ima_x = ima_x + folga_ima;
dim_ima_y = ima_y + folga_ima;
dim_ima_z = ima_z + 0.15;

/* ====================================================================
   MÓDULOS AUXILIARES DE GEOMETRIA (HULL)
   ==================================================================== */

module bloco_arredondado_2d(x, y, z, r) {
    hull() {
        translate([r, r, 0]) cylinder(r = r, h = z);
        translate([x - r, r, 0]) cylinder(r = r, h = z);
        translate([r, y - r, 0]) cylinder(r = r, h = z);
        translate([x - r, y - r, 0]) cylinder(r = r, h = z);
    }
}

module bloco_traseiro_moderno(x, y, z, r) {
    r_fundo = 1.5; 
    hull() {
        translate([r, r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([x - r, r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([r, y - r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([x - r, y - r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        
        translate([r, r, r_fundo]) sphere(r = r_fundo);
        translate([x - r, r, r_fundo]) sphere(r = r_fundo);
        translate([r, y - r, r_fundo]) sphere(r = r_fundo);
        translate([x - r, y - r, r_fundo]) sphere(r = r_fundo);
    }
}

/* ====================================================================
   PEÇAS COMPONENTES DO SANDUÍCHE
   ==================================================================== */

// 1. MOLDURA FRONTAL - PEÇA BRANCA
module moldura_frontal() {
    espessura_moldura = 3.50; 
    
    difference() {
        bloco_arredondado_2d(caixa_externa_x, caixa_externa_y, espessura_moldura, raio_canto_externo);
        
        translate([(caixa_externa_x - janela_x)/2, pos_y_visor + (berco_y - janela_y)/2, -1])
            cube([janela_x, janela_y, espessura_moldura + 2]);
        
        translate([pos_x_visor, pos_y_visor, -0.1])
            cube([berco_x, berco_y, visor_espessura + 0.1]);
            
        espacamento_botoes = caixa_externa_x / (qtd_botoes + 1);
        pos_y_botoes = 5.50 + extensao_vertical_y; 
        
        for (i = [1 : qtd_botoes]) {
            translate([espacamento_botoes * i, pos_y_botoes, -1])
                cylinder(d = botao_pino_diametro, h = espessura_moldura + 2);
                
            translate([(espacamento_botoes * i) - botao_corpo_medida/2, pos_y_botoes - botao_corpo_medida/2, -0.1])
                cube([botao_corpo_medida, botao_corpo_medida, botao_profundidade + 0.1]);
        }
        
        for(x = pos_furos_x, y = pos_furos_y) {
            translate([x, y, -1]) cylinder(d=3.2, h=espessura_moldura + 2);
        }
    }
}

// 2. BERÇO INTERMEDIÁRIO - PEÇA AMARELA
module berco_intermediario() {
    espessura_berco = 3.00; 
    
    difference() {
        bloco_arredondado_2d(caixa_externa_x, caixa_externa_y, espessura_berco, raio_canto_externo);
            
        vazado_interno_x = berco_x - 6.00;
        vazado_interno_y = berco_y - 6.00;
        translate([pos_x_visor + 3.00, pos_y_visor + 3.00, -1])
            cube([vazado_interno_x, vazado_interno_y, espessura_berco + 2]);
            
        espacamento_botoes = caixa_externa_x / (qtd_botoes + 1);
        pos_y_botoes = 5.50 + extensao_vertical_y; 
        largura_furo_botao = botao_corpo_medida + 1.5; 
        
        for (i = [1 : qtd_botoes]) {
            translate([(espacamento_botoes * i) - largura_furo_botao/2, pos_y_botoes - largura_furo_botao/2, -1])
                cube([largura_furo_botao, largura_furo_botao, espessura_berco + 2]);
        }
            
        for(x = pos_furos_x, y = pos_furos_y) {
            translate([x, y, -1]) cylinder(d=3.2, h=espessura_berco + 2);
        }
    }
}

// 3. CAIXA TRASEIRA - PEÇA VERDE (CORREÇÃO DE FLUXO SEGURO)
module caixa_traseira() {
    t_massa = dist_furo + 2.5; // Tamanho ideal da coluna nos cantos
    
    difference() {
        // --- UNIÃO: UNIMOS O CORPO OCO ÀS COLUNAS VERTICAIS INTEIRAS ---
        union() {
            // Corpo exterior com as cavidades internas já recortadas
            difference() {
                bloco_traseiro_moderno(caixa_externa_x, caixa_externa_y, caixa_profundidade, raio_canto_externo);
                
                // Cavidade interna das placas (Não corta mais as colunas pois estão fora deste sub-bloco)
                translate([parede_espessura, parede_espessura, espessura_base_traseira])
                    bloco_arredondado_2d(caixa_externa_x - (parede_espessura*2), 
                                         caixa_externa_y - (parede_espessura*2), 
                                         caixa_profundidade_interna + 1, raio_canto_externo - parede_espessura);
                                         
                // Alojamento do GPS
                translate([pos_x_gps_base, pos_y_gps_base, espessura_base_traseira])
                    cube([dim_gps_x, dim_gps_y, caixa_profundidade_interna + 1]);
                    
                // Cavidades dos ímãs
                pos_y_imas = (caixa_externa_y - dim_ima_y) / 2; 
                z_start_ima = 0.80; 
                
                translate([dist_lateral_ima + extensao_horizontal_x, pos_y_imas, z_start_ima])
                    cube([dim_ima_x, dim_ima_y, dim_ima_z]);
                    
                translate([caixa_externa_x - (dist_lateral_ima + extensao_horizontal_x) - dim_ima_x, pos_y_imas, z_start_ima])
                    cube([dim_ima_x, dim_ima_y, dim_ima_z]);
            }
            
            // Adicionamos as colunas verticais sólidas completas (de Z=0 ao topo)
            for(x = [0, caixa_externa_x - t_massa]) {
                for(y = [0, caixa_externa_y - t_massa]) {
                    intersection() {
                        // Mantém a coluna cortada sob o mesmo design arredondado exterior
                        bloco_traseiro_moderno(caixa_externa_x, caixa_externa_y, caixa_profundidade, raio_canto_externo);
                        
                        // Bloco da coluna que nasce no chão (Z=0)
                        translate([x, y, 0])
                            cube([t_massa, t_massa, caixa_profundidade]);
                    }
                }
            }
        }
        
        // --- SUBTRAÇÃO FINAL: PERFURAMOS AS COLUNAS JÁ UNIDAS ---
        // Furos cegos para os inserts (Param em Z=1.20, protegendo totalmente o acabamento do fundo)
        for(x = pos_furos_x) {
            for(y = pos_furos_y) {
                translate([x, y, 1.20]) 
                    cylinder(d = diametro_furo_insert, h = caixa_profundidade + 1);
            }
        }
    }
}

/* ====================================================================
   RENDERIZAÇÃO DO CONJUNTO
   ==================================================================== */

translate([0, 0, caixa_profundidade + 10]) color("white") moldura_frontal();
translate([0, 0, caixa_profundidade + 3]) color("yellow") berco_intermediario();
color("green") caixa_traseira();